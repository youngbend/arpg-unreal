// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "ARPGFluidDefinition.generated.h"

class UARPGMagicElement;
class UMaterialInterface;

/**
 * What a fluid is like as a BODY LYING ON THE GROUND. Port of Godot's
 * FluidDefinition.
 *
 * Distinct from the element itself, which says what the fluid IS, and from the
 * spread definition, which says how it diffuses. This is only how it behaves as
 * a puddle: how deep, how fast rain fills it and sun takes it away, how much of
 * it has to gather before it counts as bottomless.
 */
UCLASS(BlueprintType)
class ARPGWORLD_API UARPGFluidDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** What this pools. Identity, damage, status and reactions all come from it. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	TObjectPtr<UARPGMagicElement> Element;

	/** Surface height above the ground the deposit landed on. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body",
		meta = (ClampMin = "0.0"))
	float Depth = 20.f;

	/**
	 * Below this area a body is too small to be worth keeping and is removed.
	 *
	 * Not zero, because an evaporating pool's area approaches zero
	 * asymptotically -- without a floor it would live forever as a sliver,
	 * costing a rebuild every tick to become imperceptibly smaller.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body",
		meta = (ClampMin = "0.0"))
	float MinimumArea = 2500.f;

	/**
	 * Area above which the body's volume becomes a RESERVOIR -- bottomless, and
	 * damping rather than explosive when a spell lands in it.
	 *
	 * A threshold rather than a flag, so the same asset describes a splash and a
	 * lake and the difference falls out of how much has gathered.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body",
		meta = (ClampMin = "0.0"))
	float ReservoirArea = 200000.f;

	/**
	 * Mass per unit volume, in kg per cubic centimetre. Water is 0.001.
	 *
	 * What anything frozen out of this floats ON, so it is half of the answer to
	 * whether a slab rides or sinks -- the other half being the solid's own. Lava
	 * is heavy, and a crust of obsidian floating on it is a different sum from ice
	 * on water even though the code doing it is the same.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body",
		meta = (ClampMin = "0.0001"))
	float Density = 0.001f;

	// --- Running off a slab -------------------------------------------------
	//
	// ON THE FLUID AND NOT THE SLAB, which is where these started and where they
	// were wrong. Viscosity is a property of water or lava, not of the ice or
	// earth it happens to be running over: with the knob on the solid you would
	// tune "how thick is lava" inside the earth asset, and a second solid that
	// melted into lava would need the same numbers copied and kept in step.
	//
	// A slab reads these through its own MeltsInto, so it never has to know.

	/**
	 * How much of the available head the film moves per second.
	 *
	 * NOT A SPEED IN CM/S. The solver moves a fraction of the height difference
	 * between neighbouring cells, so this is a rate of settling: high is a thin
	 * quick sheet, low is a slow ooze.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Runoff",
		meta = (ClampMin = "0.0"))
	float FlowRate = 6.f;

	/**
	 * Head, in cm per cell, below which this fluid does not move at all.
	 *
	 * THE HALF OF VISCOSITY A RATE CANNOT EXPRESS, and the one that gives lava its
	 * character. A slower rate makes a fluid arrive later; a yield slope makes it
	 * STOP -- on a gradient water would sheet straight off, lava sits where it is
	 * and goes no further. It is also what makes a viscous film pile up thick
	 * rather than spreading, which falls out rather than being written: a fluid
	 * that needs more head before it moves necessarily stands deeper.
	 *
	 * Zero for water, which runs off anything.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Runoff",
		meta = (ClampMin = "0.0"))
	float YieldSlope = 0.f;

	/**
	 * Below this depth in cm a cell is dry.
	 *
	 * A FLOOR, or the film never finishes: each step moves a fraction of what is
	 * left, so depth approaches zero and never arrives, and a slab carrying a
	 * millionth of a millimetre would tick forever. A viscous fluid leaves more
	 * behind, which is also true of the real thing.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Runoff",
		meta = (ClampMin = "0.001"))
	float MinimumFilm = 0.05f;

	/**
	 * Least volume worth spawning a body for, in cubic cm, when runoff lands.
	 *
	 * Runoff arrives in dribbles by design, and every deposit is a polygon merge
	 * or an actor spawn. Without a threshold a melting tower would make that call
	 * every tick for a teaspoon.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Runoff",
		meta = (ClampMin = "0.0"))
	float RunoffBatch = 20000.f;

	/** How much energy the body's volume carries per unit of area. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body",
		meta = (ClampMin = "0.0"))
	float EnergyPerArea = 0.001f;

	/**
	 * 0-1. How well a body of this carries a charge that conducts THROUGH it.
	 *
	 * WHICH charges it carries is not a property of the fluid: that is a Conduct
	 * row in the shared combination table. This is only how well this particular
	 * substance does it, and lava should be near zero where water is high.
	 *
	 * Nothing used to set this on a pool at all -- the volume's own default is 0,
	 * and no definition carried the number -- so a real deposited puddle silently
	 * refused to conduct while the conduction tests, which set it by hand, passed.
	 * That is the phase 6 gate ("lightning floods a puddle chain and hurts a
	 * second player standing in it") failing on the one part of itself that phase
	 * 6 could not yet build.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Conductivity = 0.85f;

	/** Outward offset per second while it is raining. 0 means rain does nothing. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weather",
		meta = (ClampMin = "0.0"))
	float RainGrowthRate = 4.f;

	/** Inward offset per second otherwise. 0 means it never dries. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weather",
		meta = (ClampMin = "0.0"))
	float EvaporationRate = 1.f;

	/** How far a fresh deposit may sit from a body and still merge into it. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body",
		meta = (ClampMin = "0.0"))
	float MergeDistance = 100.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	TSoftObjectPtr<UMaterialInterface> SurfaceMaterial;

	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	FGameplayTag GetElementTag() const;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGFluid", GetFName());
	}
};
