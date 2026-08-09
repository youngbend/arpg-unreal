// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGLocomotionComponent.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

UARPGLocomotionComponent::UARPGLocomotionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
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

	// Every frame rather than only on a tier change: the tier is not the only
	// input to the speed. Designers tune WalkSpeed and the rest live in PIE, an
	// attack scales the whole thing mid-swing, and a gameplay effect elsewhere
	// can overwrite MaxWalkSpeed outright.
	ApplySpeed();
}

UAbilitySystemComponent* UARPGLocomotionComponent::GetASC() const
{
	return UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetOwner());
}

UCharacterMovementComponent* UARPGLocomotionComponent::GetMovement() const
{
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	return Character ? Character->GetCharacterMovement() : nullptr;
}

bool UARPGLocomotionComponent::TrySpendStamina(float Cost)
{
	if (Cost <= 0.f)
	{
		return true;
	}

	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC)
	{
		return true; // nothing to spend from; don't pin the character in place
	}

	const float Current = ASC->GetNumericAttribute(UARPGVitalSet::GetStaminaAttribute());
	if (Current < Cost)
	{
		return false;
	}

	ASC->SetNumericAttributeBase(UARPGVitalSet::GetStaminaAttribute(), Current - Cost);
	return true;
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
}
