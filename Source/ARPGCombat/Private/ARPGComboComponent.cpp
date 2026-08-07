// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGComboComponent.h"
#include "ARPGAttackDefinition.h"
#include "ARPGGameplayTags.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"

UARPGComboComponent::UARPGComboComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	SetIsReplicatedByDefault(false); // combo state is resolved server-side
}

UAbilitySystemComponent* UARPGComboComponent::GetASC() const
{
	if (!CachedASC)
	{
		CachedASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetOwner());
	}
	return CachedASC;
}

UARPGComboAttackNode* UARPGComboComponent::ResolveNext(EARPGAttackInput Input) const
{
	if (!AttackTree)
	{
		return nullptr;
	}

	UARPGComboAttackNode* Next = CurrentNode ? CurrentNode->GetFollow(Input) : nullptr;
	return Next ? Next : AttackTree->GetRoot(Input);
}

float UARPGComboComponent::NodeResetTimeout() const
{
	if (CurrentNode && CurrentNode->ResetTimeoutOverride > 0.f)
	{
		return CurrentNode->ResetTimeoutOverride;
	}
	return AttackTree ? AttackTree->ResetTimeout : 0.f;
}

bool UARPGComboComponent::TrySpendStamina(float Cost)
{
	if (Cost <= 0.f)
	{
		return true;
	}

	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC)
	{
		return true; // nothing to spend from; don't block the attack
	}

	const float Current = ASC->GetNumericAttribute(UARPGVitalSet::GetStaminaAttribute());
	if (Current < Cost)
	{
		return false;
	}

	ASC->SetNumericAttributeBase(UARPGVitalSet::GetStaminaAttribute(), Current - Cost);
	return true;
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
		return;
	}

	if (bAttacking)
	{
		BufferedInput = InputIndex;
		bBufferedEmpowered = bEmpowered;
		BufferTimer = BufferWindow; // measured from the press -- see the header
		return;
	}

	// A finisher's lockout blocks starting a brand-new combo, but ONLY when this
	// press would actually fall back to root. Mid-chain follow-ups, charges and
	// channels from a non-root node are untouched.
	const bool bWouldFallBackToRoot = !CurrentNode || !CurrentNode->GetFollow(Input);
	if (RootLockoutTimer > 0.f && bWouldFallBackToRoot)
	{
		return;
	}

	// Peek at the resolved attack to decide whether this is a charge or a channel.
	UARPGComboAttackNode* Next = ResolveNext(Input);
	if (!Next)
	{
		return;
	}

	UARPGAttackDefinition* Attack = Next->Attack;
	if (Attack && Attack->bChargeable)
	{
		CurrentNode = Next;
		bCharging = true;
		ChargeElapsed = 0.f;
		ChargeInput = InputIndex;
		bChargeEmpowered = bEmpowered;
		ResetTimer = 0.f;
		BufferedInput = INDEX_NONE;
		OnChargeStarted.Broadcast(CurrentNode, bEmpowered);
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
	}
}

void UARPGComboComponent::ReleaseCharge()
{
	if (!bCharging)
	{
		return;
	}

	const int32 Input = ChargeInput;
	const bool bEmpowered = bChargeEmpowered;

	bCharging = false;
	ChargeInput = INDEX_NONE;

	StartAttack(static_cast<EARPGAttackInput>(Input), bEmpowered);
}

void UARPGComboComponent::StartAttack(EARPGAttackInput Input, bool bEmpowered)
{
	if (!AttackTree)
	{
		return;
	}

	// Stops a press buffered before the lock, or a continuous-hold continuation,
	// from launching the next beat while locked.
	if (bAttackLocked)
	{
		return;
	}

	UARPGComboAttackNode* Next = nullptr;

	// Empowered at root uses the parry follow-up branch when one exists.
	if (bEmpowered && !CurrentNode)
	{
		Next = AttackTree->GetParryFollowup(Input);
	}

	if (!Next && CurrentNode)
	{
		Next = CurrentNode->GetFollow(Input);
	}

	if (!Next)
	{
		// Root fallback. Re-checked here rather than only in ReceiveInput
		// because a buffered press can be consumed the instant
		// NotifyAttackFinished sets the lockout -- which the earlier peek could
		// not have seen coming.
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
		Next = AttackTree->GetRoot(Input);
	}

	if (!Next)
	{
		return;
	}

	if (Next->Attack && !TrySpendStamina(Next->Attack->StaminaCost))
	{
		return;
	}

	CurrentNode = Next;
	bAttacking = true;
	ResetTimer = 0.f;
	BufferedInput = INDEX_NONE;
	LastInput = static_cast<int32>(Input);
	++AttackSequenceNumber;

	OnAttackStarted.Broadcast(CurrentNode, bEmpowered);

	// Hand off to the ability, which owns the montage, the tags and the hitbox
	// window. This component never touches animation.
	if (UAbilitySystemComponent* ASC = GetASC())
	{
		FGameplayEventData EventData;
		EventData.EventTag = TAG_Event_Attack_Begin;
		EventData.OptionalObject = Next->Attack;
		EventData.Instigator = GetOwner();
		EventData.Target = GetOwner();
		EventData.EventMagnitude = bEmpowered ? 1.f : 0.f;

		ASC->HandleGameplayEvent(TAG_Event_Attack_Begin, &EventData);
	}
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
}

void UARPGComboComponent::CancelAttack()
{
	bAttacking = false;
	bCharging = false;
	ChargeElapsed = 0.f;
	ChargeInput = INDEX_NONE;
	BufferedInput = INDEX_NONE;
	ResetTimer = NodeResetTimeout();
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
	ResetTimer = 0.f;
	OnComboReset.Broadcast();
}

void UARPGComboComponent::KeepAlive()
{
	if (CurrentNode && !bAttacking)
	{
		ResetTimer = NodeResetTimeout();
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
			OnComboReset.Broadcast();
		}
	}
}
