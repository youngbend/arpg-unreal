// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGGameplayAbility_Discharge.h"
#include "ARPGAbilityTask_ChargeDischarge.h"
#include "ARPGCloakComponent.h"
#include "ARPGDischargeEffect.h"
#include "ARPGGameplayTags.h"
#include "ARPGMagic.h"
#include "ARPGMagicComponent.h"
#include "ARPGMagicElement.h"
#include "AbilitySystemComponent.h"
#include "Abilities/Tasks/AbilityTask_PlayMontageAndWait.h"
#include "GameFramework/Actor.h"

UARPGGameplayAbility_Discharge::UARPGGameplayAbility_Discharge()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;

	// Predicted locally so the cast animation starts on the press, but every
	// mana spend inside the charge task is server-authoritative -- see the plan's
	// networking section. The client shows the wind-up; the server decides what
	// it cost and whether it went off.
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;

	{
		// SetAssetTags replaces rather than appends, and it is protected --
		// so a subclass carries the base class's tags forward itself.
		FGameplayTagContainer Tags = GetAssetTags();
		Tags.AddTag(TAG_Ability_Discharge);
		SetAssetTags(Tags);
	}

	ActivationBlockedTags.AddTag(TAG_State_Flinching);
	ActivationBlockedTags.AddTag(TAG_State_StanceBroken);
	ActivationBlockedTags.AddTag(TAG_State_Dead);
}

UARPGMagicComponent* UARPGGameplayAbility_Discharge::ResolveMagic() const
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	return Avatar ? Avatar->FindComponentByClass<UARPGMagicComponent>() : nullptr;
}

bool UARPGGameplayAbility_Discharge::CheckCost(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!Super::CheckCost(Handle, ActorInfo, OptionalRelevantTags))
	{
		return false;
	}

	const UARPGMagicComponent* Magic = ResolveMagic();
	if (!Magic)
	{
		return true;
	}

	// Gated on the MINIMUM, not the maximum: a discharge you can only half afford
	// is a legitimate cast that fires early, not a failed one. Matching Godot's
	// begin_discharge_charge, which refused only when even the minimum was out of
	// reach.
	return Magic->GetAvailableMana() >= Magic->GetDischargeManaCost(DischargeType, 0.f);
}

void UARPGGameplayAbility_Discharge::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	UARPGMagicComponent* Magic = ResolveMagic();
	if (!Magic)
	{
		UE_LOG(LogARPGMagic, Warning, TEXT("Discharge ability on %s, which has no magic component."),
			*GetNameSafe(GetAvatarActorFromActorInfo()));
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// Nothing readied: this press means "give me back what I just cast" rather
	// than "cast nothing". The auto-ready sequence is the whole reason repeating
	// a two-element combination is not four button presses every time.
	if (!Magic->HasActiveElements())
	{
		Magic->BeginAutoReady();
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// Two or more elements with no matching recipe. The elements stay readied --
	// the player is mid-experiment, and silently clearing their mix would cost
	// them the activation mana twice.
	if (!Magic->CanDischarge())
	{
		Magic->OnSpellFailed.Broadcast(Magic->GetActiveElementTags());
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	if (CastMontage)
	{
		UAbilityTask_PlayMontageAndWait* PlayMontage =
			UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(
				this, NAME_None, CastMontage);
		PlayMontage->ReadyForActivation();
	}

	ChargeTask = UARPGAbilityTask_ChargeDischarge::ChargeDischarge(
		this, Magic, DischargeType, MaxChargeTime);
	ChargeTask->OnReleased.AddDynamic(this, &UARPGGameplayAbility_Discharge::OnChargeReleased);
	ChargeTask->OnFailed.AddDynamic(this, &UARPGGameplayAbility_Discharge::OnChargeFailed);
	ChargeTask->ReadyForActivation();
}

void UARPGGameplayAbility_Discharge::InputReleased(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo)
{
	Super::InputReleased(Handle, ActorInfo, ActivationInfo);

	// Letting go IS the cast. Routed through the task rather than handled here so
	// there is one place that decides what fraction was reached, whether the
	// release was deliberate or forced by running dry.
	if (ChargeTask)
	{
		ChargeTask->ReleaseCharge();
	}
}

FVector UARPGGameplayAbility_Discharge::ApplyAimPitch(const FRotator& ViewRotation, float PitchOffset)
{
	FRotator Aim = ViewRotation;

	// NormalizeAxis first. A pawn's view pitch comes back in [0, 360), so a
	// caster looking twenty degrees down reports 340 -- and 340 + 15 clamps to
	// straight up rather than nudging the shot level.
	Aim.Pitch = FRotator::NormalizeAxis(Aim.Pitch) + PitchOffset;

	// Clamped short of vertical rather than wrapped: past 90 the yaw flips and
	// the bolt leaves behind the caster.
	Aim.Pitch = FMath::Clamp(Aim.Pitch, -89.f, 89.f);
	Aim.Roll = 0.f;

	return Aim.Vector();
}

FVector UARPGGameplayAbility_Discharge::GetAimDirection(const AActor& Avatar) const
{
	if (!bAimAlongView)
	{
		return Avatar.GetActorForwardVector();
	}

	// GetActorEyesViewPoint rather than the camera component: on a pawn it
	// resolves to the CONTROLLER's rotation, which the server holds for a remote
	// client (move packets carry the view) and which an AI caster fills in from
	// its focus. The camera component only exists on the machine looking through
	// it, and this runs on the server -- see OnChargeReleased.
	FVector ViewLocation;
	FRotator ViewRotation;
	Avatar.GetActorEyesViewPoint(ViewLocation, ViewRotation);

	return ApplyAimPitch(ViewRotation, AimPitchOffset);
}

void UARPGGameplayAbility_Discharge::GetSpawnTransform(FVector& OutOrigin, FVector& OutDirection) const
{
	const AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar)
	{
		OutOrigin = FVector::ZeroVector;
		OutDirection = FVector::ForwardVector;
		return;
	}

	OutDirection = GetAimDirection(*Avatar);

	// Offset along the AIM, not along a flat forward, so the effect always
	// appears on the line it is about to travel -- a bolt aimed upward that
	// spawned at chest height would visibly start off its own path.
	OutOrigin = Avatar->GetActorLocation()
		+ OutDirection * SpawnForwardOffset
		+ FVector::UpVector * SpawnHeightOffset;
}

void UARPGGameplayAbility_Discharge::OnChargeFailed(float ChargeFraction, bool bForced)
{
	if (UARPGMagicComponent* Magic = ResolveMagic())
	{
		Magic->OnSpellFailed.Broadcast(Magic->GetActiveElementTags());
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
}

void UARPGGameplayAbility_Discharge::OnChargeReleased(float ChargeFraction, bool bForced)
{
	UARPGMagicComponent* Magic = ResolveMagic();
	if (!Magic)
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
		return;
	}

	FVector Origin, Direction;
	GetSpawnTransform(Origin, Direction);

	const FARPGDischargeContext Context =
		Magic->BuildDischargeContext(DischargeType, Origin, Direction, ChargeFraction, bForced);

	// Server-only. The spawned effect owns a hitbox, so predicting it would let a
	// client conjure damage; the effect replicates down instead.
	if (HasAuthority(&CurrentActivationInfo))
	{
		PerformDischarge(Context);
		Magic->NotifyDischarged(Context);
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UARPGGameplayAbility_Discharge::PerformDischarge(const FARPGDischargeContext& Context)
{
	const UARPGMagicElement* Element = Context.PrimaryElement;
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Element || !Avatar || !Avatar->GetWorld())
	{
		return;
	}

	TSubclassOf<AActor> EffectClass = Element->ResolveDischargeEffect(DischargeType);
	if (!EffectClass)
	{
		// Already warned by ResolveDischargeEffect, which knows whether this is a
		// missing asset or a deliberate opt-out.
		return;
	}

	const FTransform SpawnTransform(Context.Direction.Rotation(), Context.Origin);

	FActorSpawnParameters Params;
	Params.Owner = Avatar;
	Params.Instigator = Cast<APawn>(Avatar);
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* Spawned = Avatar->GetWorld()->SpawnActorDeferred<AActor>(
		EffectClass, SpawnTransform, Avatar, Params.Instigator,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);

	if (!Spawned)
	{
		return;
	}

	// Deferred so the context is in place before BeginPlay arms the hitbox --
	// otherwise the first frame of a projectile is armed with default damage.
	if (AARPGDischargeEffect* Effect = Cast<AARPGDischargeEffect>(Spawned))
	{
		Effect->InitializeFromContext(Context);
	}

	Spawned->FinishSpawning(SpawnTransform);
}

// ---------------------------------------------------------------------------

UARPGGameplayAbility_DischargeBurst::UARPGGameplayAbility_DischargeBurst()
{
	DischargeType = EARPGDischargeType::Burst;
	{
		// SetAssetTags replaces rather than appends, and it is protected --
		// so a subclass carries the base class's tags forward itself.
		FGameplayTagContainer Tags = GetAssetTags();
		Tags.AddTag(TAG_Ability_Discharge_Burst);
		SetAssetTags(Tags);
	}
}

UARPGGameplayAbility_DischargeEmanate::UARPGGameplayAbility_DischargeEmanate()
{
	DischargeType = EARPGDischargeType::Emanate;
	{
		// SetAssetTags replaces rather than appends, and it is protected --
		// so a subclass carries the base class's tags forward itself.
		FGameplayTagContainer Tags = GetAssetTags();
		Tags.AddTag(TAG_Ability_Discharge_Emanate);
		SetAssetTags(Tags);
	}

	// Around the caster, not in front of them.
	SpawnForwardOffset = 0.f;
	SpawnHeightOffset = 0.f;
}

UARPGGameplayAbility_DischargeProject::UARPGGameplayAbility_DischargeProject()
{
	DischargeType = EARPGDischargeType::Project;
	{
		// SetAssetTags replaces rather than appends, and it is protected --
		// so a subclass carries the base class's tags forward itself.
		FGameplayTagContainer Tags = GetAssetTags();
		Tags.AddTag(TAG_Ability_Discharge_Project);
		SetAssetTags(Tags);
	}

	// The only discharge that TRAVELS, so the only one where being thrown along
	// the character's facing instead of the player's aim is visible: a bolt cast
	// mid-strafe left sideways. The tilt is what keeps it out of the floor --
	// the follow camera looks slightly down, and a projectile with no gravity
	// fired along that view meets the ground well short of its range.
	bAimAlongView = true;
	AimPitchOffset = 15.f;
}

UARPGGameplayAbility_DischargeCloak::UARPGGameplayAbility_DischargeCloak()
{
	DischargeType = EARPGDischargeType::Cloak;
	{
		// SetAssetTags replaces rather than appends, and it is protected --
		// so a subclass carries the base class's tags forward itself.
		FGameplayTagContainer Tags = GetAssetTags();
		Tags.AddTag(TAG_Ability_Discharge_Cloak);
		SetAssetTags(Tags);
	}
}

void UARPGGameplayAbility_DischargeCloak::PerformDischarge(const FARPGDischargeContext& Context)
{
	// Not Super: a cloak does not put anything in front of the caster. The cloak
	// component spawns and parents its own effect, because that effect has to
	// live and die with the cloak rather than on its own lifespan.
	AActor* Avatar = GetAvatarActorFromActorInfo();
	UARPGCloakComponent* Cloak = Avatar ? Avatar->FindComponentByClass<UARPGCloakComponent>() : nullptr;

	if (!Cloak)
	{
		UE_LOG(LogARPGMagic, Warning,
			TEXT("Cloak discharge on %s, which has no cloak component; nothing will be applied."),
			*GetNameSafe(Avatar));
		return;
	}

	Cloak->ApplyCloak(Context);
}
