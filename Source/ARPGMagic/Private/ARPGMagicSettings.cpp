// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGMagicSettings.h"
#include "ARPGPlaceholderEffect.h"

UARPGMagicSettings::UARPGMagicSettings()
{
	// Defaulted in code rather than shipped as config entries, so a fresh
	// checkout is visible without anyone having to discover a settings page
	// first. A config entry still wins where one exists -- these are the
	// bottom of the chain, not an override.
	PlaceholderHand = AARPGPlaceholderAttachedEffect::StaticClass();
	PlaceholderImbue = AARPGPlaceholderAttachedEffect::StaticClass();
	PlaceholderDischarge = AARPGPlaceholderDischarge::StaticClass();
	PlaceholderProjectile = AARPGPlaceholderProjectile::StaticClass();
}
