// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGMagicProgressionTracker.h"
#include "ARPGMagic.h"
#include "ARPGMagicComponent.h"
#include "ARPGMagicElement.h"
#include "GameFramework/Actor.h"

void UARPGMagicProgressionTracker::BindXPSource()
{
	AActor* Owner = GetOwner();
	UARPGMagicComponent* Magic = Owner ? Owner->FindComponentByClass<UARPGMagicComponent>() : nullptr;

	if (!Magic)
	{
		UE_LOG(LogARPGMagic, Warning,
			TEXT("Magic progression tracker on %s found no magic component, so it can never earn XP "
			     "-- but it will still GATE combinations, leaving the caster permanently unable to "
			     "combine anything."),
			*GetNameSafe(Owner));
		return;
	}

	Magic->OnDischargeExecuted.AddDynamic(
		this, &UARPGMagicProgressionTracker::HandleDischargeExecuted);
}

void UARPGMagicProgressionTracker::HandleDischargeExecuted(const FARPGDischargeContext& Context)
{
	const UARPGMagicElement* Element = Context.PrimaryElement;
	if (!Element || !Element->ElementTag.IsValid())
	{
		return;
	}

	// A combination casts as its RESULT, so steam progresses steam rather than
	// the fire and water that made it. That is what makes a combination its own
	// thing to get good at rather than a way to farm its ingredients.
	float XP = Context.ComputedDamage * XPPerDamage;

	// An element that deals no damage would otherwise never progress at all --
	// a pure debuff or heal is used, and using it is the whole basis of magic XP.
	if (XP <= 0.f && Settings)
	{
		XP = Settings->StatusApplicationXP;
	}

	RecordUse(Element->ElementTag, XP);
}

float UARPGMagicProgressionTracker::GetEffectiveElementLevel_Implementation(FGameplayTag ElementTag) const
{
	return GetEffectiveLevel(ElementTag);
}

float UARPGMagicProgressionTracker::GetElementDamageMultiplier_Implementation(FGameplayTag ElementTag) const
{
	return GetMultiplier(ElementTag);
}
