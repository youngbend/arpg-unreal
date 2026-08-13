// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "ARPGSolidDefinition.generated.h"

class UARPGFluidDefinition;
class UARPGMagicElement;
class UMaterialInterface;

/**
 * What an element is like as a SOLID frozen out of a fluid. Port of Godot's
 * SolidDefinition.
 *
 * Nothing here knows that water freezes: WHICH pairs solidify is a Solidify row
 * in the combination table's Surface scope -- the same table and the same
 * scoping every other relationship in the game uses. So ice + water making a
 * floe and lava + water making a crust of obsidian are two rows and two assets,
 * and neither is a branch in any C++ file.
 */
UCLASS(BlueprintType)
class ARPGWORLD_API UARPGSolidDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** What this is made of -- matched against a Solidify row's result. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	TObjectPtr<UARPGMagicElement> Element;

	/** How far the slab stands above the fluid surface it formed on. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body",
		meta = (ClampMin = "0.0"))
	float Thickness = 30.f;

	/**
	 * Whether the slab carries collision.
	 *
	 * The whole point for ice, and deliberately optional: a crust of obsidian
	 * over lava should be standable, a sheet of frost should not.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body")
	bool bStandable = true;

	/**
	 * How fast time alone takes this, in cm of thickness per second. 0 is
	 * permanent -- obsidian is rock, not frozen lava.
	 *
	 * ONE OF TWO INDEPENDENT QUESTIONS, and they are worth keeping apart. This one
	 * is "does the world wear it away". The other is "can something MELT it", which
	 * is EnergyPerArea: a slab with no energy density is not a body a reaction can
	 * eat, however much fire is thrown at it. Obsidian answers no to both; ice
	 * answers yes to both; a magical ward might sit still forever and still be
	 * broken by a big enough spell.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melting",
		meta = (ClampMin = "0.0"))
	float MeltRate = 0.5f;

	/**
	 * What the slab turns back into as it melts, if anything.
	 *
	 * Null is right for obsidian, which is permanent rock rather than frozen
	 * lava. Ice points back at water, so a floe melting returns its area to the
	 * pool it came from rather than the fluid simply vanishing.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melting")
	TObjectPtr<UARPGFluidDefinition> MeltsInto;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body",
		meta = (ClampMin = "0.0"))
	float MinimumArea = 2500.f;

	/**
	 * How much energy a unit of this slab's area is worth, which is what lets a
	 * fire spell MELT it rather than merely waiting for MeltRate to.
	 *
	 * A slab used to carry no energy at all -- "a thing you stand on, not a body
	 * you react with" -- and the consequence was that the reaction solver bailed
	 * at its own guard against zero-energy volumes, so a fireball thrown at an ice
	 * floe did precisely nothing.
	 *
	 * Higher than water's on purpose: ice takes more energy to shift per unit of
	 * ground than the same ground of open water, so one fireball opens a hole
	 * rather than clearing the floe.
	 *
	 * ZERO MEANS NOTHING CAN MELT IT. That is the setting for permanent rock, and
	 * it is a separate question from MeltRate -- see that field.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body",
		meta = (ClampMin = "0.0"))
	float EnergyPerArea = 0.002f;

	/**
	 * How coarse the slab's heightfield is, in cm.
	 *
	 * THE ONLY RESOLUTION KNOB a floe has, and the one that decides what it costs:
	 * triangles, collision cook and replication all scale with the cell count, and
	 * the cell count is the slab's area over the square of this. Smaller reads
	 * smoother and costs quadratically more.
	 *
	 * Also the floor on detail: a bowl melted narrower than a cell shows up as one
	 * cell going down, and a hole cannot be finer than this.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body",
		meta = (ClampMin = "5.0"))
	float CellSize = 20.f;

	/**
	 * How wide a bowl one fire impact melts, in cm.
	 *
	 * The energy decides how DEEP; this decides how broad, and the two together
	 * are what make a fireball at the edge take an angled bite while the same one
	 * in the middle drills through. Narrow and deep punches holes; wide and
	 * shallow dishes the surface.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melting",
		meta = (ClampMin = "1.0"))
	float MeltRadius = 90.f;

	// --- Floating ---------------------------------------------------------------
	//
	// A slab frozen on water RIDES it, and how it rides is Archimedes with two
	// deliberate departures for feel. Real ice floats 92% submerged, which leaves
	// a couple of centimetres of freeboard on a slab you are meant to walk on; and
	// a real person on a ten-square-metre floe pushes it down under a centimetre,
	// which nobody would ever see. Density and LoadResponse are where those two
	// are traded away, and everything else is the honest equation.

	/**
	 * Mass per unit volume, in kg per cubic centimetre. Water is 0.001.
	 *
	 * Below the density of what it formed on, or it does not float -- and a slab
	 * that does not float RESTS ON THE BED rather than sitting awash, which is one
	 * branch on the same equation and is how a crust denser than its own fluid
	 * behaves without anything being special-cased for it.
	 *
	 * DELIBERATELY LIGHTER THAN REAL ICE (0.00092): the physical value leaves a
	 * 30cm slab riding with 2cm proud, which is a surface awash rather than one
	 * you would choose to stand on.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Floating",
		meta = (ClampMin = "0.0"))
	float Density = 0.0006f;

	/** What one thing standing on it weighs, in kg. Characters do not simulate. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Floating",
		meta = (ClampMin = "0.0"))
	float OccupantMass = 80.f;

	/**
	 * Exaggerates how far a load pushes the slab down.
	 *
	 * 1 is physically honest and almost invisible -- see the section note. This is
	 * the one number that is openly a lie, and it is a lie in service of the thing
	 * the player is supposed to feel: that the ice gives under them.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Floating",
		meta = (ClampMin = "1.0"))
	float LoadResponse = 12.f;

	/** How quickly it settles to the depth it should be riding at, per second. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Floating",
		meta = (ClampMin = "0.1"))
	float SettleSpeed = 3.f;

	/**
	 * How fast a RAISED slab climbs out of the ground, in the same units.
	 *
	 * Separate from SettleSpeed because they are opposite kinds of motion. Settling
	 * is continuous and wants to feel soft -- a surface giving under a footfall.
	 * Rising happens once and wants to feel violent: a wall of earth easing
	 * gracefully into place is not the spell anyone cast.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Raising",
		meta = (ClampMin = "0.1"))
	float RiseSpeed = 14.f;

	/**
	 * How much of the current it takes, 0-1.
	 *
	 * 1 means it travels at exactly the speed of the water. Below that it lags,
	 * which is what a heavy slab against a fast river actually does.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Floating",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DriftResponse = 0.6f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	TSoftObjectPtr<UMaterialInterface> SurfaceMaterial;

	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	FGameplayTag GetElementTag() const;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGSolid", GetFName());
	}
};
