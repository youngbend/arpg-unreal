// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGLocomotionComponent.h"
#include "ARPGAttributeLibrary.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

UARPGLocomotionComponent::UARPGLocomotionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	// In a cooked build the only per-frame work here is the sprint's stamina
	// drain, so the tick follows the sprint. In the editor it stays on so that
	// WalkSpeed and friends can be tuned live in PIE and take effect immediately.
#if WITH_EDITOR
	PrimaryComponentTick.bStartWithTickEnabled = true;
#else
	PrimaryComponentTick.bStartWithTickEnabled = false;
#endif

	SetIsReplicatedByDefault(false);
}

void UARPGLocomotionComponent::BeginPlay()
{
	Super::BeginPlay();

	LastBroadcastTier = GetSpeedTier();
	ApplySpeed();
}

void UARPGLocomotionComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bSprinting && SprintStaminaDrain > 0.f && !TrySpendStamina(SprintStaminaDrain * DeltaTime))
	{
		// Out of stamina: the sprint ends here and now. Deliberately NOT put on
		// a cooldown -- pulling the stick again and getting a couple of steps
		// out of whatever regenerated is the correct read of an exhausted
		// character, and a lockout would only teach the player to stop trying.
		SetSprinting(false);
	}

	// Reasserted every frame in the editor so live tuning of WalkSpeed and the
	// rest is visible immediately. In a cooked build every input to the speed --
	// tier, walk-forced, attack scaling -- routes through RefreshTier, which
	// applies it on the spot.
	//
	// This used to run unconditionally, and the melee ability wrote MaxWalkSpeed
	// directly, so the two fought and the ability always lost. The ability now
	// goes through SetAttackMovement, which makes this the single writer.
#if WITH_EDITOR
	ApplySpeed();
#endif
}

void UARPGLocomotionComponent::RefreshTickState()
{
#if !WITH_EDITOR
	// Only the sprint drain needs a per-frame tick.
	SetComponentTickEnabled(bSprinting && SprintStaminaDrain > 0.f);
#endif
}

UCharacterMovementComponent* UARPGLocomotionComponent::GetMovement() const
{
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	return Character ? Character->GetCharacterMovement() : nullptr;
}

bool UARPGLocomotionComponent::TrySpendStamina(float Cost)
{
	// Shared helper: this ran every frame of every sprint, reading the CURRENT
	// stamina and writing it back as the BASE, so any stamina buff active during
	// a sprint was permanently baked into the base value.
	return UARPGAttributeLibrary::TrySpend(GetASC(), UARPGVitalSet::GetStaminaAttribute(), Cost);
}

// ---------------------------------------------------------------------------
// Sprint
// ---------------------------------------------------------------------------

void UARPGLocomotionComponent::SetSprinting(bool bNewSprinting)
{
	// Refused rather than accepted-and-immediately-dropped, so IsSprinting()
	// never reads true for a frame in a state that forbids it -- an animation
	// blend or a UI element reading it mid-frame would flicker.
	if (bNewSprinting && (bWalkForced || !bSprintAllowed))
	{
		bNewSprinting = false;
	}

	if (bNewSprinting == bSprinting)
	{
		return;
	}

	bSprinting = bNewSprinting;
	RefreshTier();
}

void UARPGLocomotionComponent::SetMoveMagnitude(float Magnitude)
{
	MoveMagnitude = FMath::Clamp(Magnitude, 0.f, 1.f);

	if (MoveMagnitude < 0.001f && bSprinting)
	{
		SetSprinting(false);
	}
}

// ---------------------------------------------------------------------------
// Tier
// ---------------------------------------------------------------------------

void UARPGLocomotionComponent::SetWalkForced(bool bForced)
{
	if (bForced == bWalkForced)
	{
		return;
	}

	bWalkForced = bForced;

	if (bWalkForced && bSprinting)
	{
		bSprinting = false;
	}

	RefreshTier();
}

void UARPGLocomotionComponent::SetAttackMovement(float Factor, bool bAllowSprint)
{
	AttackSpeedFactor = FMath::Max(0.f, Factor);
	bSprintAllowed = bAllowSprint;

	if (!bSprintAllowed && bSprinting)
	{
		bSprinting = false;
	}

	RefreshTier();
}

EARPGSpeedTier UARPGLocomotionComponent::GetSpeedTier() const
{
	// Walk wins outright. A forced walk is a restriction, not a preference, so
	// it outranks a sprint the player asked for -- and SetSprinting refuses
	// while it holds, so the two cannot disagree for longer than one call.
	if (bWalkForced)
	{
		return EARPGSpeedTier::Walk;
	}

	return bSprinting ? EARPGSpeedTier::Sprint : EARPGSpeedTier::Run;
}

float UARPGLocomotionComponent::GetCurrentSpeed() const
{
	float Base = RunSpeed;
	switch (GetSpeedTier())
	{
	case EARPGSpeedTier::Walk:   Base = WalkSpeed;   break;
	case EARPGSpeedTier::Sprint: Base = SprintSpeed; break;
	default: break;
	}

	return Base * AttackSpeedFactor;
}

void UARPGLocomotionComponent::ApplySpeed()
{
	if (UCharacterMovementComponent* Movement = GetMovement())
	{
		Movement->MaxWalkSpeed = GetCurrentSpeed();
	}
}

void UARPGLocomotionComponent::RefreshTier()
{
	const EARPGSpeedTier Tier = GetSpeedTier();
	if (Tier != LastBroadcastTier)
	{
		LastBroadcastTier = Tier;
		OnSpeedTierChanged.Broadcast(Tier);
	}

	ApplySpeed();
	RefreshTickState();
}
