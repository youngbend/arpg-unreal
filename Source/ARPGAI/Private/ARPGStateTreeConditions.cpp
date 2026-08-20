// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGStateTreeConditions.h"
#include "ARPGStateTreeContext.h"
#include "ARPGVitalSet.h"
#include "ARPGWeaponComponent.h"
#include "ARPGWeaponDefinition.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "GameFramework/Pawn.h"

// ---------------------------------------------------------------------------
// Attack range
// ---------------------------------------------------------------------------

bool FARPGStateTreeCondition_TargetInAttackRange::TestCondition(
	FStateTreeExecutionContext& Context) const
{
	const FInstanceDataType& Data = Context.GetInstanceData(*this);

	const APawn* Pawn = ARPGStateTree::GetPawn(Context);
	if (!Pawn || !Data.Target)
	{
		return false;
	}

	// Reach comes from the equipped weapon, so one tree serves a dagger NPC and
	// a greatsword one without either needing its own copy.
	float Reach = ReachTolerance;
	if (const UARPGWeaponComponent* Weapon = Pawn->FindComponentByClass<UARPGWeaponComponent>())
	{
		if (const UARPGWeaponDefinition* Definition = Weapon->GetWeapon())
		{
			Reach += Definition->Reach;
		}
	}

	return FVector::Dist(Pawn->GetActorLocation(), Data.Target->GetActorLocation()) <= Reach;
}

// ---------------------------------------------------------------------------
// Facing
// ---------------------------------------------------------------------------

bool FARPGStateTreeCondition_FacingTarget::TestCondition(FStateTreeExecutionContext& Context) const
{
	const FInstanceDataType& Data = Context.GetInstanceData(*this);

	const APawn* Pawn = ARPGStateTree::GetPawn(Context);
	if (!Pawn || !Data.Target)
	{
		return false;
	}

	// Horizontal only. A target on a ledge above is still "faced" -- verticality
	// is the aim system's problem, not the decision to swing.
	const FVector ToTarget =
		(Data.Target->GetActorLocation() - Pawn->GetActorLocation()).GetSafeNormal2D();
	if (ToTarget.IsNearlyZero())
	{
		return true;
	}

	const FVector Facing = Pawn->GetActorForwardVector().GetSafeNormal2D();
	const float AngleCos = FMath::Cos(FMath::DegreesToRadians(MaxAngleDegrees));

	return FVector::DotProduct(Facing, ToTarget) >= AngleCos;
}

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------

bool FARPGStateTreeCondition_HealthBelow::TestCondition(FStateTreeExecutionContext& Context) const
{
	const APawn* Pawn = ARPGStateTree::GetPawn(Context);

	const UAbilitySystemComponent* ASC =
		UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Pawn);
	if (!ASC)
	{
		return false;
	}

	const float MaxHealth = ASC->GetNumericAttribute(UARPGVitalSet::GetMaxHealthAttribute());
	if (MaxHealth <= 0.f)
	{
		// Guarding the divide rather than treating it as "definitely hurt": a
		// character with no max health is misconfigured, and sending it to flee
		// and heal would hide that.
		return false;
	}

	const float Health = ASC->GetNumericAttribute(UARPGVitalSet::GetHealthAttribute());
	return (Health / MaxHealth) < Threshold;
}

// ---------------------------------------------------------------------------
// Leash
// ---------------------------------------------------------------------------

bool FARPGStateTreeCondition_IsLeashed::TestCondition(FStateTreeExecutionContext& Context) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);

	const APawn* Pawn = ARPGStateTree::GetPawn(Context);
	if (!Pawn)
	{
		return false;
	}

	const float Distance = FVector::Dist(Pawn->GetActorLocation(), Data.HomeLocation);

	// Hysteresis: once leashed, stay leashed until back inside the smaller
	// radius. Without it an NPC sitting exactly on the boundary flips between
	// chasing and returning every tick, which reads as a twitch.
	if (Data.bLeashed)
	{
		const float Release = ReleaseRange > 0.f ? ReleaseRange : LeashRange;
		Data.bLeashed = Distance > Release;
	}
	else
	{
		Data.bLeashed = Distance > LeashRange;
	}

	return Data.bLeashed;
}
