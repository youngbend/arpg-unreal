// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGGameplayAbility_Dodge.h"
#include "ARPGDischargeEffect.h"
#include "ARPGDodgeData.h"
#include "ARPGGameplayTags.h"
#include "ARPGMagic.h"
#include "ARPGMagicComponent.h"
#include "ARPGMagicElement.h"
#include "AbilitySystemComponent.h"
#include "Abilities/Tasks/AbilityTask_PlayMontageAndWait.h"
#include "Abilities/Tasks/AbilityTask_WaitDelay.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

UARPGGameplayAbility_Dodge::UARPGGameplayAbility_Dodge()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;

	AbilityTags.AddTag(TAG_Ability_Dodge);
	ActivationOwnedTags.AddTag(TAG_State_Dodging);

	ActivationBlockedTags.AddTag(TAG_State_StanceBroken);
	ActivationBlockedTags.AddTag(TAG_State_Dead);

	// Deliberately NOT blocked by State.Flinching: dodging out of a flinch is
	// how a player escapes a combo, and blocking it would make any hit a
	// guaranteed follow-up.
}

FVector UARPGGameplayAbility_Dodge::ResolveDodgeDirection(bool& bOutIsBackstep) const
{
	const ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character)
	{
		bOutIsBackstep = true;
		return FVector::ForwardVector;
	}

	const FVector Input = Character->GetLastMovementInputVector().GetSafeNormal2D();

	// No directional input is a backstep, not a forward dodge -- so a panicked
	// press while standing still moves the player away from what they are facing.
	bOutIsBackstep = Input.IsNearlyZero();
	return bOutIsBackstep ? -Character->GetActorForwardVector() : Input;
}

void UARPGGameplayAbility_Dodge::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	UARPGMagicComponent* Magic = Avatar->FindComponentByClass<UARPGMagicComponent>();

	// Consumed on the server only -- it spends mana. The client still runs the
	// rest of the ability, and the element replicates through the magic
	// component's mask, so the dodge is not held up waiting for the round trip.
	if (Magic && HasAuthority(&ActivationInfo) && Magic->HasActiveElements())
	{
		DodgeElement = Magic->ConsumeForDodge();
	}

	// An element with no dodge data authored is NOT an elemental dodge. Falling
	// back to standard is the honest outcome: the alternative is a dodge with
	// zero distance and no i-frames that looks like a bug.
	const UARPGElementalDodgeData* Elemental =
		DodgeElement ? DodgeElement->ElementalDodge.Get() : nullptr;

	bool bIsBackstep = false;
	const FVector Direction = ResolveDodgeDirection(bIsBackstep);

	float Distance = 400.f;
	float Duration = 0.25f;
	float InvincibilityStart = 0.f;
	float InvincibilityDuration = 0.f;
	FVector Impulse = FVector::ZeroVector;
	UAnimMontage* Montage = nullptr;

	if (Elemental)
	{
		Distance = Elemental->Distance;
		Duration = Elemental->Duration;
		InvincibilityDuration = Elemental->InvincibilityWindow;
		Montage = Elemental->Montage;

		// Impulse is authored in DODGE-RELATIVE space: +X along the dodge, +Y to
		// its right, +Z world up. Rotating it here is what lets one authored
		// vector read the same whichever way the player rolls.
		const FRotator DodgeFrame = Direction.Rotation();
		Impulse = DodgeFrame.RotateVector(Elemental->Impulse);

		SpawnDodgeEffect(*Elemental);
	}
	else if (StandardDodge)
	{
		Distance = StandardDodge->Distance;
		Duration = StandardDodge->Duration;
		InvincibilityStart = StandardDodge->InvincibilityWindowStart;
		InvincibilityDuration = StandardDodge->InvincibilityWindowDuration;
		Montage = (bIsBackstep && StandardDodge->BackstepMontage)
			? StandardDodge->BackstepMontage
			: StandardDodge->Montage;
	}

	// 0 means the whole dodge, for both data types.
	if (InvincibilityDuration <= 0.f)
	{
		InvincibilityDuration = Duration - InvincibilityStart;
	}

	BeginMovement(Distance, Duration, Impulse);

	if (Montage)
	{
		UAbilityTask_PlayMontageAndWait* PlayMontage =
			UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(
				this, NAME_None, Montage);
		PlayMontage->ReadyForActivation();
	}

	if (InvincibilityStart > 0.f)
	{
		UAbilityTask_WaitDelay* Start = UAbilityTask_WaitDelay::WaitDelay(this, InvincibilityStart);
		Start->OnFinish.AddDynamic(this, &UARPGGameplayAbility_Dodge::OnInvincibilityStart);
		Start->ReadyForActivation();
	}
	else
	{
		OnInvincibilityStart();
	}

	UAbilityTask_WaitDelay* IFramesEnd =
		UAbilityTask_WaitDelay::WaitDelay(this, InvincibilityStart + InvincibilityDuration);
	IFramesEnd->OnFinish.AddDynamic(this, &UARPGGameplayAbility_Dodge::OnInvincibilityEnd);
	IFramesEnd->ReadyForActivation();

	UAbilityTask_WaitDelay* Finished = UAbilityTask_WaitDelay::WaitDelay(this, Duration);
	Finished->OnFinish.AddDynamic(this, &UARPGGameplayAbility_Dodge::OnDodgeFinished);
	Finished->ReadyForActivation();
}

void UARPGGameplayAbility_Dodge::BeginMovement(float Distance, float Duration, const FVector& Impulse)
{
	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character || Duration <= 0.f)
	{
		return;
	}

	bool bIsBackstep = false;
	const FVector Direction = ResolveDodgeDirection(bIsBackstep);

	// Distance over duration gives the speed that actually covers the authored
	// distance, so retiming a dodge does not silently change how far it goes.
	const FVector Velocity = Direction * (Distance / Duration) + Impulse;

	// bXYOverride so the dodge replaces existing lateral velocity rather than
	// adding to it -- a dodge from a sprint would otherwise travel much further
	// than one from standing.
	Character->LaunchCharacter(Velocity, /*bXYOverride=*/true, /*bZOverride=*/!Impulse.Z);
}

void UARPGGameplayAbility_Dodge::SpawnDodgeEffect(const UARPGElementalDodgeData& Data)
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !Avatar->GetWorld() || Data.DodgeEffect.IsNull())
	{
		return;
	}

	// Server-only: the effect owns a hitbox, and a client-spawned one would be
	// conjuring damage.
	if (!HasAuthority(&CurrentActivationInfo))
	{
		return;
	}

	UClass* EffectClass = Data.DodgeEffect.LoadSynchronous();
	if (!EffectClass)
	{
		UE_LOG(LogARPGMagic, Warning, TEXT("Dodge effect '%s' would not load."),
			*Data.DodgeEffect.ToString());
		return;
	}

	FActorSpawnParameters Params;
	Params.Owner = Avatar;
	Params.Instigator = Cast<APawn>(Avatar);
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	DodgeEffect = Avatar->GetWorld()->SpawnActor<AActor>(
		EffectClass, Avatar->GetActorTransform(), Params);

	// A dodge effect that happens to be a discharge effect gets the same damage
	// provisioning every spell does, scaled by the dodge's own multiplier.
	if (AARPGDischargeEffect* Discharge = Cast<AARPGDischargeEffect>(DodgeEffect))
	{
		FARPGDischargeContext Context;
		Context.DischargeType = EARPGDischargeType::Burst;
		Context.Caster = Avatar;
		Context.PrimaryElement = DodgeElement;
		Context.Origin = Avatar->GetActorLocation();
		Context.Direction = Avatar->GetActorForwardVector();
		Context.ComputedDamage = DodgeElement ? DodgeElement->BaseDamage * Data.DamageMultiplier : 0.f;
		Context.ComputedPoiseDamage = DodgeElement ? DodgeElement->BasePoiseDamage * Data.DamageMultiplier : 0.f;

		Discharge->InitializeFromContext(Context);
	}
}

void UARPGGameplayAbility_Dodge::OnInvincibilityStart()
{
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->AddLooseGameplayTag(TAG_State_Invulnerable, 1, EGameplayTagReplicationState::TagOnly);
		bGrantedInvincibility = true;
	}
}

void UARPGGameplayAbility_Dodge::OnInvincibilityEnd()
{
	if (!bGrantedInvincibility)
	{
		return;
	}

	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->RemoveLooseGameplayTag(TAG_State_Invulnerable);
	}
	bGrantedInvincibility = false;
}

void UARPGGameplayAbility_Dodge::OnDodgeFinished()
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UARPGGameplayAbility_Dodge::EndAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility, bool bWasCancelled)
{
	// On every exit path, so a dodge cancelled mid-roll cannot leave the
	// character permanently invulnerable -- the single worst bug this ability
	// could ship with.
	OnInvincibilityEnd();

	DodgeElement = nullptr;
	DodgeEffect = nullptr; // owns its own lifetime

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}
