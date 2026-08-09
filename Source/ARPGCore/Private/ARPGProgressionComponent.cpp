// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGProgressionComponent.h"
#include "ARPGCore.h"
#include "ARPGVitalSet.h"
#include "ARPGXPCurve.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"

UARPGProgressionComponent::UARPGProgressionComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false); // server state; the owning client reads it via UI
}

void UARPGProgressionComponent::BeginPlay()
{
	Super::BeginPlay();
	CaptureBaseValues();
}

UAbilitySystemComponent* UARPGProgressionComponent::GetASC() const
{
	if (!CachedASC)
	{
		CachedASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetOwner());
	}
	return CachedASC;
}

void UARPGProgressionComponent::CaptureBaseValues()
{
	if (bCapturedBase)
	{
		return;
	}

	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC)
	{
		// Retried on the next allocation. Capturing zeroes here would permanently
		// reset the character's maxima to whatever their points alone provide.
		return;
	}

	BaseMaxHealth = ASC->GetNumericAttribute(UARPGVitalSet::GetMaxHealthAttribute());
	BaseMaxStamina = ASC->GetNumericAttribute(UARPGVitalSet::GetMaxStaminaAttribute());
	BaseMaxMana = ASC->GetNumericAttribute(UARPGVitalSet::GetMaxManaAttribute());
	bCapturedBase = true;
}

float UARPGProgressionComponent::GetXPIntoLevel() const
{
	return XPCurve ? XPCurve->GetRemainderForTotalXP(TotalXP) : TotalXP;
}

float UARPGProgressionComponent::GetXPToNextLevel() const
{
	return XPCurve ? XPCurve->GetXPToNext(Level) : 0.f;
}

int32 UARPGProgressionComponent::GetAllocatedPoints(EARPGAttributePoint Which) const
{
	switch (Which)
	{
	case EARPGAttributePoint::Stamina: return AllocatedStamina;
	case EARPGAttributePoint::Mana:    return AllocatedMana;
	case EARPGAttributePoint::Health:
	default:                           return AllocatedHealth;
	}
}

int32& UARPGProgressionComponent::AllocationFor(EARPGAttributePoint Which)
{
	switch (Which)
	{
	case EARPGAttributePoint::Stamina: return AllocatedStamina;
	case EARPGAttributePoint::Mana:    return AllocatedMana;
	case EARPGAttributePoint::Health:
	default:                           return AllocatedHealth;
	}
}

void UARPGProgressionComponent::GrantXP(float Amount)
{
	if (Amount <= 0.f)
	{
		return;
	}

	TotalXP += Amount;
	OnXPGained.Broadcast(Amount, TotalXP);

	if (!XPCurve)
	{
		return;
	}

	// Re-solved from the total rather than stepped, so a single large grant that
	// crosses several levels awards points for all of them.
	int32 NewLevel = Level;
	float Remainder = 0.f;
	XPCurve->SolveLevel(TotalXP, NewLevel, Remainder);

	if (NewLevel <= Level)
	{
		return;
	}

	const int32 LevelsGained = NewLevel - Level;
	const int32 PointsGranted = LevelsGained * PointsPerLevel;

	Level = NewLevel;
	AvailablePoints += PointsGranted;

	OnLevelUp.Broadcast(Level, PointsGranted);
	OnPointsChanged.Broadcast(AvailablePoints);
}

bool UARPGProgressionComponent::SpendPoint(EARPGAttributePoint Which)
{
	if (AvailablePoints <= 0)
	{
		return false;
	}

	--AvailablePoints;
	++AllocationFor(Which);

	ReapplyAllocations();
	OnPointsChanged.Broadcast(AvailablePoints);
	return true;
}

void UARPGProgressionComponent::ReapplyAllocations()
{
	CaptureBaseValues();

	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC || !bCapturedBase)
	{
		return;
	}

	// ABSOLUTE, not additive. Re-applying this after a load has to land on the
	// same number, and a delta would compound every point on every load.
	ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxHealthAttribute(),
		BaseMaxHealth + AllocatedHealth * HealthPerPoint);
	ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxStaminaAttribute(),
		BaseMaxStamina + AllocatedStamina * StaminaPerPoint);
	ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxManaAttribute(),
		BaseMaxMana + AllocatedMana * ManaPerPoint);
}

void UARPGProgressionComponent::RestoreProgress(int32 InLevel, float InTotalXP,
	int32 InAvailablePoints, int32 InHealthPoints, int32 InStaminaPoints, int32 InManaPoints)
{
	Level = FMath::Max(1, InLevel);
	TotalXP = FMath::Max(0.f, InTotalXP);
	AvailablePoints = FMath::Max(0, InAvailablePoints);

	AllocatedHealth = FMath::Max(0, InHealthPoints);
	AllocatedStamina = FMath::Max(0, InStaminaPoints);
	AllocatedMana = FMath::Max(0, InManaPoints);

	ReapplyAllocations();
	OnPointsChanged.Broadcast(AvailablePoints);
}
