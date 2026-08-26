// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ARPGWorldSettings.generated.h"

class UARPGMagicCombinationTable;
class UARPGSpreadDefinition;
class UARPGSpreadFuelMap;
class UARPGFluidDefinition;
class UARPGSolidDefinition;
class UMaterialParameterCollection;
class UNiagaraSystem;

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

	/** What each element is like as a body lying on the ground. No entry, no pool. */
	UPROPERTY(EditAnywhere, Config, Category = "Fluids")
	TArray<TSoftObjectPtr<UARPGFluidDefinition>> FluidDefinitions;

	/**
	 * What each element is like frozen out of one of those bodies.
	 *
	 * Separate from the fluids because a Solidify row names its PRODUCT, and ice
	 * is not a thing that pools -- an unlisted product warns and freezes nothing.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Fluids")
	TArray<TSoftObjectPtr<UARPGSolidDefinition>> SolidDefinitions;

	// --- What a fluid LOOKS like -----------------------------------------------
	//
	// SEPARATE FROM THE DEFINITIONS ABOVE, and separate on the same rule the
	// solvers already follow: a fluid definition says what water is like as a
	// body -- how deep, how fast it dries, how well it conducts -- and every one
	// of those is a number the simulation reads. These two are the machinery that
	// DRAWS it, they are read only by UARPGFluidPresentationSubsystem, and a world
	// with neither set simulates exactly the same water and shows you nothing.
	//
	// Which is why they are here rather than on each definition: there is one
	// sheet for the whole world, not one per element. Water and lava differ in
	// the MATERIAL they are drawn with, which is on the definition, not in the
	// grid they are simulated on.

	/** The Grid2D shallow-water sim the visible surface is drawn from. */
	UPROPERTY(EditAnywhere, Config, Category = "Fluid Presentation")
	TSoftObjectPtr<UNiagaraSystem> FluidSheetSystem;

	/** Where the sheet's window is, for the surface material to line up with. */
	UPROPERTY(EditAnywhere, Config, Category = "Fluid Presentation")
	TSoftObjectPtr<UMaterialParameterCollection> FluidParameters;
};
