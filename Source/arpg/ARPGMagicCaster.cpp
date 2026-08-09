// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGMagicCaster.h"
#include "ARPGCloakComponent.h"
#include "ARPGMagicComponent.h"

AARPGMagicCaster::AARPGMagicCaster()
{
	Magic = CreateDefaultSubobject<UARPGMagicComponent>(TEXT("Magic"));
	Cloak = CreateDefaultSubobject<UARPGCloakComponent>(TEXT("Cloak"));
}

float AARPGMagicCaster::GetEffectiveElementLevel_Implementation(FGameplayTag ElementTag) const
{
	// Unlisted is 0, not 1: an element the caster has never trained is one they
	// cannot yet combine with anything, which is the gate's whole purpose.
	const float* Found = ElementLevels.Find(ElementTag);
	return Found ? *Found : 0.f;
}

float AARPGMagicCaster::GetElementDamageMultiplier_Implementation(FGameplayTag ElementTag) const
{
	// Unlisted is 1 here, because an untrained element still casts -- it just
	// casts without a mastery bonus.
	const float* Found = ElementDamageMultipliers.Find(ElementTag);
	return Found ? *Found : 1.f;
}
