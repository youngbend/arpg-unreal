// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGComboComponent.h"
#include "ARPGAttackDefinition.h"
#include "ARPGAttributeLibrary.h"
#include "ARPGCombat.h"
#include "ARPGGameplayTags.h"
#include "ARPGSwingAugment.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"

UARPGComboComponent::UARPGComboComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	SetIsReplicatedByDefault(false); // combo state is resolved server-side
}

void UARPGComboComponent::RefreshTickState()
{
	const bool bNeedsTick = bCharging
		|| bChannelLooping
		|| BufferedInput != INDEX_NONE
		|| RootLockoutTimer > 0.f
		|| (!bAttacking && CurrentNode && ResetTimer > 0.f);

	SetComponentTickEnabled(bNeedsTick);
}

UARPGComboAttackNode* UARPGComboComponent::ResolveAttackOverride(EARPGAttackInput Input)
{
	UObject* Augment = ARPGSwingAugments::FindActive(GetASC());
	if (!Augment)
	{
		return nullptr;
	}

	UARPGAttackDefinition* Attack =
		IARPGSwingAugment::Execute_GetSwingAttackOverride(Augment, Input);
	if (!Attack)
	{
		return nullptr;
	}

	if (!OverrideNode)
	{
		OverrideNode = NewObject<UARPGComboAttackNode>(this);
	}

	// Follows stay null for the life of the component, which is what makes this a
	// leaf and therefore what ends the chain.
	OverrideNode->Attack = Attack;
	return OverrideNode;
}

UARPGComboAttackNode* UARPGComboComponent::ResolveNext(EARPGAttackInput Input, bool bEmpowered,
	bool& bOutFallsBackToRoot)
{
	bOutFallsBackToRoot = false;

	// FIRST, and ahead of the tree entirely. Something readied that performs its
	// own attack replaces this beat wherever the chain happens to be -- that is
	// the whole point of it, and checking the tree first would let a weapon's
	// follow-up win over the move the player has already paid mana for.
	//
	// bOutFallsBackToRoot stays false: an override is not re-entry to root, so a
	// finisher's lockout does not eat a move that has already been paid for.
	if (UARPGComboAttackNode* Override = ResolveAttackOverride(Input))
	{
		return Override;
	}

	UARPGWeaponAttackTree* Tree = GetActiveTree();
	if (!Tree)
	{
		return nullptr;
	}

	// Empowered at root takes the parry follow-up branch when one exists. Checked
	// FIRST, and by both callers, which is the whole reason this is one function.
	if (bEmpowered && !CurrentNode)
	{
		if (UARPGComboAttackNode* Followup = Tree->GetParryFollowup(Input))
		{
			return Followup;
		}
	}

	if (CurrentNode)
	{
		if (UARPGComboAttackNode* Follow = CurrentNode->GetFollow(Input))
		{
			return Follow;
		}
	}

	bOutFallsBackToRoot = true;
	return Tree->GetRoot(Input);
}

float UARPGComboComponent::NodeResetTimeout() const
{
	if (CurrentNode && CurrentNode->ResetTimeoutOverride > 0.f)
	{
		return CurrentNode->ResetTimeoutOverride;
	}
	const UARPGWeaponAttackTree* Tree = GetActiveTree();
	return Tree ? Tree->ResetTimeout : 0.f;
}

bool UARPGComboComponent::TrySpendStamina(float Cost)
{
	// Shared with locomotion, magic and the ability tasks. The four had a copy
	// each, and all four read the CURRENT value and wrote it back as the BASE.
	return UARPGAttributeLibrary::TrySpend(GetASC(), UARPGVitalSet::GetStaminaAttribute(), Cost);
}

void UARPGComboComponent::ReceiveInput(EARPGAttackInput Input, bool bEmpowered)
{
	const int32 InputIndex = static_cast<int32>(Input);
	if (InputIndex < 0 || InputIndex >= static_cast<int32>(EARPGAttackInput::MAX))
	{
		return;
	}

	bInputHeld[InputIndex] = true;

	if (bCharging)
	{
		return; // new presses are ignored while charging
	}

	// A fresh press during flinch recovery is dropped outright. This also blocks
	// what would have been a new charge or channel, neither of which routes
	// through StartAttack below.
	if (bAttackLocked && !bAttacking)
	{
		UE_LOG(LogARPGCombat, Verbose, TEXT("%s: attack input dropped, attack locked (flinching)."),
			*GetNameSafe(GetOwner()));
		return;
	}

	if (bAttacking)
	{
		BufferedInput = InputIndex;
		bBufferedEmpowered = bEmpowered;
		BufferTimer = BufferWindow; // measured from the press -- see the header
		RefreshTickState();
		return;
	}

	// Peek at the resolved attack to decide whether this is a charge or a
	// channel. The SAME resolution StartAttack will perform, including the
	// empowered parry-follow-up branch -- see ResolveNext.
	bool bWouldFallBackToRoot = false;
	UARPGComboAttackNode* Next = ResolveNext(Input, bEmpowered, bWouldFallBackToRoot);

	// A finisher's lockout blocks starting a brand-new combo, but ONLY when this
	// press would actually fall back to root. Mid-chain follow-ups, charges and
	// channels from a non-root node are untouched.
	if (RootLockoutTimer > 0.f && bWouldFallBackToRoot)
	{
		return;
	}

	if (!Next)
	{
		// Silence here is what makes a misconfigured tree look like broken input.
		UE_LOG(LogARPGCombat, Warning,
			TEXT("%s: no attack resolved for input %d (%s). %s"),
			*GetNameSafe(GetOwner()), InputIndex,
			CurrentNode ? TEXT("mid-combo") : TEXT("from root"),
			GetActiveTree() ? TEXT("The tree has no node for this input.")
			                : TEXT("NO ATTACK TREE: nothing is equipped and no "
			                       "FallbackAttackTree is set."));
		return;
	}

	UARPGAttackDefinition* Attack = Next->Attack;
	if (Attack && Attack->bChargeable)
	{
		CurrentNode = Next;
		bCharging = true;
		ChargeElapsed = 0.f;
		ChargeFraction = 0.f;
		ChargeInput = InputIndex;
		bChargeEmpowered = bEmpowered;
		ResetTimer = 0.f;
		BufferedInput = INDEX_NONE;
		LastInput = InputIndex;
		RefreshTickState();
		OnChargeStarted.Broadcast(CurrentNode, bEmpowered);

		// The ability starts NOW, not on release: the wind-up has to be on screen
		// while the player holds, both as the feedback that tells them how far
		// they have charged and as the tell an opponent reads. Stamina is not
		// spent yet -- an interrupted charge should cost nothing.
		SendAttackBeginEvent(bEmpowered);
		return;
	}

	if (Attack && Attack->bChannel)
	{
		StartChannel(Next, InputIndex, bEmpowered);
		return;
	}

	StartAttack(Input, bEmpowered);
}

void UARPGComboComponent::ReceiveInputReleased(EARPGAttackInput Input)
{
	const int32 InputIndex = static_cast<int32>(Input);
	if (InputIndex < 0 || InputIndex >= static_cast<int32>(EARPGAttackInput::MAX))
	{
		return;
	}

	bInputHeld[InputIndex] = false;

	if (bCharging && ChargeInput == InputIndex)
	{
		ReleaseCharge();
		return;
	}

	if (bChanneling && ChannelInput == InputIndex)
	{
		bChannelReleaseRequested = true;
		if (bChannelLooping)
		{
			EndChannelLoop();
		}
		// Otherwise still in the wind-up, and NotifyChannelWindupFinished will
		// see the request and decline to start the loop.
	}
}

void UARPGComboComponent::ReleaseCharge()
{
	if (!bCharging)
	{
		return;
	}

	UARPGAttackDefinition* Attack = CurrentNode ? CurrentNode->Attack : nullptr;
	const float ChargeTime = Attack ? Attack->ChargeTime : 0.f;

	// A zero charge time would divide by zero; treat it as instantly full, which
	// is the only reading of "chargeable over no time" that makes sense.
	ChargeFraction = ChargeTime > 0.f
		? FMath::Clamp(ChargeElapsed / ChargeTime, 0.f, 1.f)
		: 1.f;

	bCharging = false;
	ChargeInput = INDEX_NONE;
	ResetTimer = 0.f;
	BufferedInput = INDEX_NONE;

	// Deferred from the press. Releasing without the stamina to swing cancels the
	// whole thing rather than producing a free attack.
	if (Attack && !TrySpendStamina(Attack->StaminaCost))
	{
		UE_LOG(LogARPGCombat, Log, TEXT("%s: charged '%s' fizzled, needs %.0f stamina."),
			*GetNameSafe(GetOwner()), *Attack->AttackId.ToString(), Attack->StaminaCost);
		CancelAttack();
		if (UAbilitySystemComponent* ASC = GetASC())
		{
			FGameplayTagContainer AttackTags;
			AttackTags.AddTag(TAG_Ability_Attack_Melee);
			ASC->CancelAbilities(&AttackTags);
		}
		return;
	}

	bAttacking = true;
	++AttackSequenceNumber;
	RefreshTickState();
	OnAttackStarted.Broadcast(CurrentNode, bChargeEmpowered);

	// The ability is already running the wind-up. This tells it to cut to the
	// active window, and at what charge -- it does not start a second ability.
	SendAbilityEvent(TAG_Event_Attack_ChargeRelease, ChargeFraction);
}

// ---------------------------------------------------------------------------

void UARPGComboComponent::StartChannel(UARPGComboAttackNode* Node, int32 InputIndex, bool bEmpowered)
{
	UARPGAttackDefinition* Attack = Node ? Node->Attack : nullptr;
	if (!Attack)
	{
		return;
	}

	// Unlike a charge, a channel commits its opening cost at the press: the
	// wind-up is the attack starting, not a wind-up the player can back out of.
	if (!TrySpendStamina(Attack->StaminaCost))
	{
		UE_LOG(LogARPGCombat, Log, TEXT("%s: channel '%s' denied, needs %.0f stamina."),
			*GetNameSafe(GetOwner()), *Attack->AttackId.ToString(), Attack->StaminaCost);
		return;
	}

	CurrentNode = Node;
	bChanneling = true;
	bChannelLooping = false;
	ChannelInput = InputIndex;
	bChannelEmpowered = bEmpowered;
	bChannelReleaseRequested = false;
	bAttacking = true;
	ResetTimer = 0.f;
	BufferedInput = INDEX_NONE;
	LastInput = InputIndex;
	++AttackSequenceNumber;
	RefreshTickState();

	OnAttackStarted.Broadcast(CurrentNode, bEmpowered);
	SendAttackBeginEvent(bEmpowered);
}

bool UARPGComboComponent::NotifyChannelWindupFinished()
{
	if (!bChanneling || bChannelLooping)
	{
		return false;
	}

	// Tapped rather than held. The wind-up still played -- the player committed
	// that much -- but nothing loops.
	const bool bStillHeld = ChannelInput != INDEX_NONE && bInputHeld[ChannelInput];
	if (bChannelReleaseRequested || !bStillHeld)
	{
		OnChannelEnded.Broadcast(CurrentNode, bChannelEmpowered);
		return false;
	}

	bChannelLooping = true;
	RefreshTickState();
	OnChannelLoopStarted.Broadcast(CurrentNode, bChannelEmpowered);
	return true;
}

bool UARPGComboComponent::TickChannelLoop(float DeltaTime)
{
	if (!bChanneling || !bChannelLooping)
	{
		return false;
	}

	UARPGAttackDefinition* Attack = CurrentNode ? CurrentNode->Attack : nullptr;
	const float Rate = Attack ? Attack->ChannelStaminaPerSecond : 0.f;

	if (Rate > 0.f && !TrySpendStamina(Rate * DeltaTime))
	{
		EndChannelLoop();
		return false;
	}

	return true;
}

void UARPGComboComponent::EndChannelLoop()
{
	if (!bChanneling || !bChannelLooping)
	{
		return;
	}

	bChannelLooping = false;
	RefreshTickState();
	OnChannelEnded.Broadcast(CurrentNode, bChannelEmpowered);

	// Tells the running ability to break out of the looping Active section and
	// fall through to its recovery.
	SendAbilityEvent(TAG_Event_Attack_ChannelStop, 0.f);
}

void UARPGComboComponent::NotifyChannelEnded()
{
	if (!bChanneling)
	{
		return;
	}

	// A channel's recovery is a recovery like any other, so the same finisher
	// lockout applies -- see NotifyAttackFinished.
	if (CurrentNode && CurrentNode->IsLeaf() && CurrentNode->Attack)
	{
		const float Lockout = CurrentNode->Attack->FinisherLockout;
		if (Lockout > 0.f)
		{
			RootLockoutTimer = Lockout;
		}
	}

	bChanneling = false;
	bChannelLooping = false;
	bChannelReleaseRequested = false;
	ChannelInput = INDEX_NONE;
	bAttacking = false;

	if (BufferedInput != INDEX_NONE)
	{
		const EARPGAttackInput Input = static_cast<EARPGAttackInput>(BufferedInput);
		const bool bEmpowered = bBufferedEmpowered;
		BufferedInput = INDEX_NONE;
		StartAttack(Input, bEmpowered);
		return;
	}

	ResetTimer = NodeResetTimeout();
	RefreshTickState();
}

// ---------------------------------------------------------------------------

void UARPGComboComponent::SendAttackBeginEvent(bool bEmpowered)
{
	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC || !CurrentNode || !CurrentNode->Attack)
	{
		return;
	}

	FGameplayEventData EventData;
	EventData.EventTag = TAG_Event_Attack_Begin;
	EventData.OptionalObject = CurrentNode->Attack;
	EventData.Instigator = GetOwner();
	EventData.Target = GetOwner();
	EventData.EventMagnitude = bEmpowered ? 1.f : 0.f;

	ASC->HandleGameplayEvent(TAG_Event_Attack_Begin, &EventData);
}

void UARPGComboComponent::SendAbilityEvent(const FGameplayTag& EventTag, float Magnitude)
{
	if (UAbilitySystemComponent* ASC = GetASC())
	{
		FGameplayEventData EventData;
		EventData.EventTag = EventTag;
		EventData.Instigator = GetOwner();
		EventData.Target = GetOwner();
		EventData.EventMagnitude = Magnitude;

		ASC->HandleGameplayEvent(EventTag, &EventData);
	}
}

void UARPGComboComponent::StartAttack(EARPGAttackInput Input, bool bEmpowered)
{
	if (!GetActiveTree())
	{
		UE_LOG(LogARPGCombat, Warning, TEXT("%s: cannot attack, no attack tree assigned."),
			*GetNameSafe(GetOwner()));
		return;
	}

	// Stops a press buffered before the lock, or a continuous-hold continuation,
	// from launching the next beat while locked.
	if (bAttackLocked)
	{
		return;
	}

	bool bFallsBackToRoot = false;
	UARPGComboAttackNode* Next = ResolveNext(Input, bEmpowered, bFallsBackToRoot);

	if (bFallsBackToRoot)
	{
		// Re-checked here rather than only in ReceiveInput because a buffered
		// press can be consumed the instant NotifyAttackFinished sets the
		// lockout -- which the earlier peek could not have seen coming.
		if (RootLockoutTimer > 0.f)
		{
			return;
		}

		const bool bWasInCombo = CurrentNode != nullptr;
		CurrentNode = nullptr;
		if (bWasInCombo)
		{
			OnComboReset.Broadcast();
		}
	}

	if (!Next)
	{
		return;
	}

	if (!Next->Attack)
	{
		UE_LOG(LogARPGCombat, Warning,
			TEXT("%s: resolved a combo node with no AttackDefinition assigned."),
			*GetNameSafe(GetOwner()));
		return;
	}

	if (Next->Attack && !TrySpendStamina(Next->Attack->StaminaCost))
	{
		UE_LOG(LogARPGCombat, Log, TEXT("%s: '%s' denied, needs %.0f stamina."),
			*GetNameSafe(GetOwner()), *Next->Attack->AttackId.ToString(),
			Next->Attack->StaminaCost);
		return;
	}

	// Chain depth, snapshotted for the beat about to run. The root-fallback
	// branch above has already cleared CurrentNode where it applies, but the
	// counter is reset here rather than there so both the fresh-chain and
	// continuation cases are visible in one place.
	if (bFallsBackToRoot)
	{
		BeatsThisChain = 0;
	}
	ChainDepth = BeatsThisChain;

	// An override ends the chain rather than extending it, so it does not count
	// itself. That is what lets it be paid for what the player had already built
	// without also inflating the next thing to read this.
	if (Next != OverrideNode)
	{
		++BeatsThisChain;
	}

	CurrentNode = Next;
	bAttacking = true;
	ResetTimer = 0.f;
	BufferedInput = INDEX_NONE;
	LastInput = static_cast<int32>(Input);
	++AttackSequenceNumber;
	RefreshTickState();

	UE_LOG(LogARPGCombat, Log, TEXT("%s: beat %d '%s' (montage %s)"),
		*GetNameSafe(GetOwner()), AttackSequenceNumber,
		Next->Attack ? *Next->Attack->AttackId.ToString() : TEXT("<no attack>"),
		Next->Attack && Next->Attack->Montage ? *Next->Attack->Montage->GetName()
		                                      : TEXT("<NONE - nothing will play>"));

	OnAttackStarted.Broadcast(CurrentNode, bEmpowered);

	// An uncharged swing is at full charge by definition, so anything reading the
	// fraction gets 1 rather than whatever the last charge happened to leave.
	ChargeFraction = 1.f;

	// Hand off to the ability, which owns the montage, the tags and the hitbox
	// window. This component never touches animation.
	SendAttackBeginEvent(bEmpowered);
}

void UARPGComboComponent::NotifyAttackFinished()
{
	bAttacking = false;

	// Latch the finisher lockout BEFORE anything below can clear CurrentNode.
	// Only a true leaf counts -- the same attack can be a leaf in one branch and
	// mid-chain in another, so tree position gates this, not a flag.
	if (CurrentNode && CurrentNode->IsLeaf() && CurrentNode->Attack)
	{
		const float Lockout = CurrentNode->Attack->FinisherLockout;
		if (Lockout > 0.f)
		{
			RootLockoutTimer = Lockout;
		}
	}

	if (BufferedInput != INDEX_NONE)
	{
		const EARPGAttackInput Input = static_cast<EARPGAttackInput>(BufferedInput);
		const bool bEmpowered = bBufferedEmpowered;
		BufferedInput = INDEX_NONE;
		StartAttack(Input, bEmpowered);
		return;
	}

	// Continuous hold: keep the chain going with no fresh press, as long as the
	// button is still down.
	UARPGAttackDefinition* Attack = CurrentNode ? CurrentNode->Attack : nullptr;
	if (Attack && Attack->bContinuousHold && LastInput != INDEX_NONE && bInputHeld[LastInput])
	{
		StartAttack(static_cast<EARPGAttackInput>(LastInput), false);
		if (bAttacking)
		{
			return; // auto-continued
		}
		// Stamina denied or no node resolved -- fall through to the reset timer.
	}

	ResetTimer = NodeResetTimeout();
	RefreshTickState();
}

void UARPGComboComponent::CancelAttack()
{
	bAttacking = false;
	bCharging = false;
	ChargeElapsed = 0.f;
	ChargeInput = INDEX_NONE;

	// A channel interrupted mid-loop must not leave bChanneling set: the ability
	// that would have called NotifyChannelEnded is the thing being cancelled, so
	// nothing else would ever clear it and every later press would be swallowed.
	bChanneling = false;
	bChannelLooping = false;
	bChannelReleaseRequested = false;
	ChannelInput = INDEX_NONE;

	BufferedInput = INDEX_NONE;
	ResetTimer = NodeResetTimeout();
	RefreshTickState();
}

void UARPGComboComponent::ResetCombo()
{
	// Deliberately silent when already at root, so observers do not see a reset
	// that did not happen.
	if (!CurrentNode)
	{
		return;
	}

	CurrentNode = nullptr;
	BeatsThisChain = 0;
	ResetTimer = 0.f;
	RefreshTickState();
	OnComboReset.Broadcast();
}

void UARPGComboComponent::KeepAlive()
{
	if (CurrentNode && !bAttacking)
	{
		ResetTimer = NodeResetTimeout();
		RefreshTickState();
	}
}

void UARPGComboComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Expire a stale buffered press. Ticked before the charging early-return so
	// it keeps counting down regardless of phase.
	if (BufferedInput != INDEX_NONE)
	{
		BufferTimer -= DeltaTime;
		if (BufferTimer <= 0.f)
		{
			BufferedInput = INDEX_NONE;
		}
	}

	if (RootLockoutTimer > 0.f)
	{
		RootLockoutTimer = FMath::Max(0.f, RootLockoutTimer - DeltaTime);
	}

	// Drains while the loop runs, and ends the channel the moment stamina runs
	// out. Deliberately before the charging early-return: the two are mutually
	// exclusive, but ordering it this way means a future attack that somehow set
	// both flags still drains rather than looping for free.
	if (bChannelLooping)
	{
		TickChannelLoop(DeltaTime);
	}

	if (bCharging)
	{
		ChargeElapsed += DeltaTime;
		if (CurrentNode && CurrentNode->Attack &&
			ChargeElapsed >= CurrentNode->Attack->ChargeTime)
		{
			ReleaseCharge(); // auto-release at full charge
		}
		return; // the reset timer does not run while charging
	}

	if (!bAttacking && CurrentNode && ResetTimer > 0.f)
	{
		ResetTimer -= DeltaTime;
		if (ResetTimer <= 0.f)
		{
			CurrentNode = nullptr;
			BeatsThisChain = 0;
			OnComboReset.Broadcast();
		}
	}

	// Everything above counts down; once none of it is live there is nothing to
	// tick for until the next press.
	RefreshTickState();
}
