// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGGameplayAbility_MeleeAttack.h"
#include "ARPGAttackDefinition.h"
#include "ARPGCombat.h"
#include "ARPGComboComponent.h"
#include "ARPGGameplayTags.h"
#include "ARPGHitboxComponent.h"
#include "AbilitySystemComponent.h"
#include "Abilities/Tasks/AbilityTask_PlayMontageAndWait.h"
#include "Abilities/Tasks/AbilityTask_WaitGameplayEvent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

UARPGGameplayAbility_MeleeAttack::UARPGGameplayAbility_MeleeAttack()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;

	AbilityTags.AddTag(TAG_Ability_Attack);
	AbilityTags.AddTag(TAG_Ability_Attack_Melee);
	ActivationOwnedTags.AddTag(TAG_State_Attacking);

	// Declarative replacements for plumbing the Godot version did by hand:
	// ComboComponent::set_attack_locked() became a blocked tag, and the death
	// gate an explicit one.
	ActivationBlockedTags.AddTag(TAG_State_Flinching);
	ActivationBlockedTags.AddTag(TAG_State_StanceBroken);
	ActivationBlockedTags.AddTag(TAG_State_Dead);

	FAbilityTriggerData Trigger;
	Trigger.TriggerTag = TAG_Event_Attack_Begin;
	Trigger.TriggerSource = EGameplayAbilityTriggerSource::GameplayEvent;
	AbilityTriggers.Add(Trigger);
}

void UARPGGameplayAbility_MeleeAttack::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	CurrentAttack = TriggerEventData
		? Cast<UARPGAttackDefinition>(TriggerEventData->OptionalObject.Get())
		: nullptr;

	if (!CurrentAttack)
	{
		UE_LOG(LogARPGCombat, Warning,
			TEXT("Melee attack triggered with no UARPGAttackDefinition on the event."));
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	if (!CurrentAttack->Montage)
	{
		// Not fatal to the combo: end cleanly so the chain can still advance
		// rather than leaving the component stuck mid-beat.
		UE_LOG(LogARPGCombat, Warning, TEXT("Attack '%s' has no montage assigned."),
			*CurrentAttack->AttackId.ToString());
		if (UARPGComboComponent* Combo = ResolveCombo())
		{
			Combo->NotifyAttackFinished();
		}
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	bComboWindowOpened = false;

	if (CurrentAttack->bHyperarmor)
	{
		if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
		{
			ASC->AddLooseGameplayTag(TAG_State_Hyperarmor, 1,
				EGameplayTagReplicationState::TagOnly);
			bGrantedHyperarmor = true;
		}
	}

	// Rooting and slowing are part of an attack's weight, and the Godot version
	// applied MovementSpeedFactor for exactly the attack's duration.
	if (ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo()))
	{
		if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
		{
			CachedMaxWalkSpeed = Movement->MaxWalkSpeed;
			Movement->MaxWalkSpeed *= CurrentAttack->MovementSpeedFactor;
			bAppliedSpeedFactor = true;
		}
	}

	// The notifies drive everything from here. Bound before the montage starts,
	// so a hitbox notify sitting on the first frame is not missed.
	UAbilityTask_WaitGameplayEvent* HitboxOn =
		UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(this, TAG_Event_Attack_HitboxOn);
	HitboxOn->EventReceived.AddDynamic(this, &UARPGGameplayAbility_MeleeAttack::OnHitboxOn);
	HitboxOn->ReadyForActivation();

	UAbilityTask_WaitGameplayEvent* HitboxOff =
		UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(this, TAG_Event_Attack_HitboxOff);
	HitboxOff->EventReceived.AddDynamic(this, &UARPGGameplayAbility_MeleeAttack::OnHitboxOff);
	HitboxOff->ReadyForActivation();

	UAbilityTask_WaitGameplayEvent* ComboWindow =
		UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(this, TAG_Event_Attack_ComboWindow);
	ComboWindow->EventReceived.AddDynamic(this, &UARPGGameplayAbility_MeleeAttack::OnComboWindow);
	ComboWindow->ReadyForActivation();

	UAbilityTask_PlayMontageAndWait* PlayMontage =
		UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(
			this, NAME_None, CurrentAttack->Montage);
	PlayMontage->OnCompleted.AddDynamic(this, &UARPGGameplayAbility_MeleeAttack::OnMontageFinished);
	PlayMontage->OnBlendOut.AddDynamic(this, &UARPGGameplayAbility_MeleeAttack::OnMontageFinished);
	PlayMontage->OnInterrupted.AddDynamic(this, &UARPGGameplayAbility_MeleeAttack::OnMontageCancelled);
	PlayMontage->OnCancelled.AddDynamic(this, &UARPGGameplayAbility_MeleeAttack::OnMontageCancelled);
	PlayMontage->ReadyForActivation();
}

// ---------------------------------------------------------------------------

UARPGHitboxComponent* UARPGGameplayAbility_MeleeAttack::ResolveHitbox() const
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !CurrentAttack)
	{
		return nullptr;
	}

	TArray<UARPGHitboxComponent*> Hitboxes;
	Avatar->GetComponents<UARPGHitboxComponent>(Hitboxes);

	for (UARPGHitboxComponent* Hitbox : Hitboxes)
	{
		if (Hitbox->HitboxSource == CurrentAttack->HitboxSource)
		{
			return Hitbox;
		}
	}

	// A kick authored for the body hitbox on a character that only has a weapon
	// one would otherwise silently deal no damage.
	if (Hitboxes.Num() > 0)
	{
		UE_LOG(LogARPGCombat, Warning,
			TEXT("Attack '%s' wants hitbox source %d but %s has none; using the first available."),
			*CurrentAttack->AttackId.ToString(),
			static_cast<int32>(CurrentAttack->HitboxSource), *Avatar->GetName());
		return Hitboxes[0];
	}

	return nullptr;
}

UARPGComboComponent* UARPGGameplayAbility_MeleeAttack::ResolveCombo() const
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	return Avatar ? Avatar->FindComponentByClass<UARPGComboComponent>() : nullptr;
}

void UARPGGameplayAbility_MeleeAttack::ArmHitbox(int32 WindowIndex, bool bLandingWindow)
{
	UARPGHitboxComponent* Hitbox = ResolveHitbox();
	if (!Hitbox || !CurrentAttack)
	{
		return;
	}

	// Motion value selects which of the attack's damage scalars this window uses:
	// a double-hit swing's second window and an air attack's landing impact hit
	// for different amounts than the opening blow.
	const float MotionValue = bLandingWindow
		? CurrentAttack->LandingMotionValue
		: (WindowIndex > 0 ? CurrentAttack->MotionValue2 : CurrentAttack->MotionValue);

	if (MotionValue <= 0.f)
	{
		return; // a landing beat authored as pure visual, with no hit
	}

	Hitbox->BaseDamage = Hitbox->WeaponBaseDamage * MotionValue;
	Hitbox->PoiseDamage = CurrentAttack->GetEffectivePoiseDamage(MotionValue);
	Hitbox->KnockbackForce = CurrentAttack->KnockbackPower;
	Hitbox->HitStopDuration = CurrentAttack->HitStopDuration;
	Hitbox->TickInterval = CurrentAttack->HitboxTickInterval;
	Hitbox->bUnblockable = CurrentAttack->bUnblockable;

	if (CurrentAttack->DamageTypeOverride)
	{
		Hitbox->DamageType = CurrentAttack->DamageTypeOverride;
	}

	// Attribution: the swing belongs to the character, not to whatever actor the
	// hitbox happens to be parented to.
	Hitbox->SetSourceActor(GetAvatarActorFromActorInfo());

	Hitbox->ActivateHitbox();
	ArmedHitbox = Hitbox;
}

void UARPGGameplayAbility_MeleeAttack::DisarmHitbox()
{
	if (ArmedHitbox)
	{
		ArmedHitbox->DeactivateHitbox();
		ArmedHitbox = nullptr;
	}
}

// ---------------------------------------------------------------------------

void UARPGGameplayAbility_MeleeAttack::OnHitboxOn(FGameplayEventData Payload)
{
	// The notify packs the window index into EventMagnitude, with -1 meaning a
	// landing window. See UARPGAnimNotifyState_Hitbox.
	const float Magnitude = Payload.EventMagnitude;
	ArmHitbox(FMath::Max(0, FMath::RoundToInt(Magnitude)), Magnitude < 0.f);
}

void UARPGGameplayAbility_MeleeAttack::OnHitboxOff(FGameplayEventData Payload)
{
	DisarmHitbox();
}

void UARPGGameplayAbility_MeleeAttack::OnComboWindow(FGameplayEventData Payload)
{
	if (bComboWindowOpened)
	{
		return; // a looping channel montage can pass the notify more than once
	}
	bComboWindowOpened = true;

	// Hands control back: the component fires a buffered press, auto-chains a
	// continuous-hold attack, or starts the reset timer. The ability does not
	// decide which.
	if (UARPGComboComponent* Combo = ResolveCombo())
	{
		Combo->NotifyAttackFinished();
	}
}

void UARPGGameplayAbility_MeleeAttack::OnMontageFinished()
{
	// A montage that ends without ever reaching its combo notify still has to
	// release the chain, or the character is stuck mid-combo until the reset
	// timer expires. Common on an attack whose notify was never placed.
	if (!bComboWindowOpened)
	{
		bComboWindowOpened = true;
		if (UARPGComboComponent* Combo = ResolveCombo())
		{
			Combo->NotifyAttackFinished();
		}
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UARPGGameplayAbility_MeleeAttack::OnMontageCancelled()
{
	// Interrupted by a dodge, a flinch or death. CancelAttack rather than
	// NotifyAttackFinished: the beat did not complete, so nothing should chain
	// off it.
	if (UARPGComboComponent* Combo = ResolveCombo())
	{
		Combo->CancelAttack();
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
}

// ---------------------------------------------------------------------------

void UARPGGameplayAbility_MeleeAttack::RestoreCharacter()
{
	if (bAppliedSpeedFactor)
	{
		if (ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo()))
		{
			if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
			{
				Movement->MaxWalkSpeed = CachedMaxWalkSpeed;
			}
		}
		bAppliedSpeedFactor = false;
	}

	if (bGrantedHyperarmor)
	{
		if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
		{
			ASC->RemoveLooseGameplayTag(TAG_State_Hyperarmor);
		}
		bGrantedHyperarmor = false;
	}
}

void UARPGGameplayAbility_MeleeAttack::EndAbility(
	const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility, bool bWasCancelled)
{
	// Runs on every exit path including cancellation, which is what guarantees a
	// cancelled swing cannot leave a live hitbox, permanent hyperarmor, or a
	// character stuck at a rooted attack's walk speed.
	DisarmHitbox();
	RestoreCharacter();
	CurrentAttack = nullptr;

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}
