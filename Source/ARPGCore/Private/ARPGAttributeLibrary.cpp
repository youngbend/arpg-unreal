// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGAttributeLibrary.h"
#include "AbilitySystemComponent.h"

bool UARPGAttributeLibrary::TrySpend(UAbilitySystemComponent* ASC,
	const FGameplayAttribute& Attribute, float Cost)
{
	if (Cost <= 0.f)
	{
		return true;
	}

	if (!ASC)
	{
		// No ability system means no pool to spend from. Treated as unlimited
		// rather than as empty -- see the header.
		return true;
	}

	// Affordability against the CURRENT value: a temporary stamina buff is
	// stamina, and refusing to spend it would make the buff decorative.
	if (ASC->GetNumericAttribute(Attribute) < Cost)
	{
		return false;
	}

	// The write lands on the BASE. Taking the current value here would fold
	// every active modifier into the base permanently.
	const float Base = ASC->GetNumericAttributeBase(Attribute);
	ASC->SetNumericAttributeBase(Attribute, FMath::Max(0.f, Base - Cost));
	return true;
}

void UARPGAttributeLibrary::AddToBase(UAbilitySystemComponent* ASC,
	const FGameplayAttribute& Attribute, float Delta, float Min /* = -MAX_flt for unclamped */)
{
	if (!ASC || Delta == 0.f)
	{
		return;
	}

	const float Base = ASC->GetNumericAttributeBase(Attribute);
	ASC->SetNumericAttributeBase(Attribute, FMath::Max(Min, Base + Delta));
}

void UARPGAttributeLibrary::SetBase(UAbilitySystemComponent* ASC,
	const FGameplayAttribute& Attribute, float Value)
{
	if (ASC)
	{
		ASC->SetNumericAttributeBase(Attribute, Value);
	}
}

float UARPGAttributeLibrary::GetBase(const UAbilitySystemComponent* ASC,
	const FGameplayAttribute& Attribute)
{
	return ASC ? ASC->GetNumericAttributeBase(Attribute) : 0.f;
}

float UARPGAttributeLibrary::GetCurrent(const UAbilitySystemComponent* ASC,
	const FGameplayAttribute& Attribute)
{
	return ASC ? ASC->GetNumericAttribute(Attribute) : 0.f;
}
