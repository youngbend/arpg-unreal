// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ARPGWorldSettings.generated.h"

class UARPGMagicCombinationTable;
class UARPGSpreadDefinition;
class UARPGSpreadFuelMap;
class UARPGFluidDefinition;

/**
 * Authoring surface for the four elemental solvers.
 *
 * WHY THIS EXISTS. The solvers are UWorldSubsystems, and their configuration
 * lived on them as UPROPERTY(EditAnywhere) fields -- which have no editing
 * surface at all, because a world subsystem is never placed, never selected and
 * never serialised. In practice the only things that ever set them were the
 * automation tests, so in a real session fire spread, elemental reactions and
 * conduction were all running with no definitions and no combination table:
 * inert, silently, with nothing to look at that would say so.
 *
 * Developer settings are the same shape the magic module already uses for its
 * placeholder VFX (UARPGMagicSettings), and they are config-backed, so a
 * designer can point these at content without a rebuild.
 *
 * SOFT REFERENCES throughout. These are read once when a subsystem initialises,
 * which happens on world load; hard references here would pull the whole
 * combination table and every spread definition into memory before the first map
 * is even chosen. Tests keep assigning the subsystem fields directly, which
 * still wins -- see UARPGSpreadSubsystem::Initialize.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "ARPG World"))
class ARPGWORLD_API UARPGWorldSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Game"); }

	/** Convenience accessor; never null, as this is a CDO-backed settings object. */
	static const UARPGWorldSettings& Get();

	/**
	 * The one table every solver reads -- the player's hand, two spells meeting,
	 * two media sharing ground, and a charge entering a medium.
	 *
	 * One asset on purpose: fire + water is steam in all four places, and a
	 * second rule set to keep in sync is what this avoids.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Solvers",
		meta = (AllowedClasses = "/Script/ARPGMagic.ARPGMagicCombinationTable"))
	TSoftObjectPtr<UARPGMagicCombinationTable> CombinationTable;

	// --- Spread ---------------------------------------------------------------

	/** Every diffusive medium the world simulates: fire, corruption, pestilence. */
	UPROPERTY(EditAnywhere, Config, Category = "Spread")
	TArray<TSoftObjectPtr<UARPGSpreadDefinition>> SpreadDefinitions;

	/** Baked per-chunk fuel. Leave unset for uniformly flammable ground. */
	UPROPERTY(EditAnywhere, Config, Category = "Spread")
	TSoftObjectPtr<UARPGSpreadFuelMap> FuelMap;

	// --- Fluids ---------------------------------------------------------------

	UPROPERTY(EditAnywhere, Config, Category = "Fluids")
	TArray<TSoftObjectPtr<UARPGFluidDefinition>> FluidDefinitions;
};
