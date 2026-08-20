// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "Chaos/ChaosEngineInterface.h"
#include "ARPGSpreadDefinition.generated.h"

class UARPGMagicElement;

/**
 * How ONE KIND OF THING answers to a spreadable medium: how hard it is to
 * light, and how long it burns. Port of Godot's SpreadProfile.
 *
 * Grass and a tree are the case this exists for. They differ in ways that are
 * properties of the GRASS AND THE TREE, not of fire: grass catches almost at
 * once and is gone in seconds, an oak resists a while then burns for most of a
 * minute. Without this the only lever was fire RESISTANCE on the target, which
 * also made it take less damage from fireballs -- so "slow to catch but burns to
 * nothing once lit" could not be said at all.
 */
UCLASS(BlueprintType)
class ARPGWORLD_API UARPGSpreadProfile : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Which medium this describes. A profile with no element is ignored. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profile",
		meta = (Categories = "Element"))
	FGameplayTag ElementTag;

	/**
	 * Multiplies the medium's ignition threshold for this object. Above 1 is
	 * harder to light.
	 *
	 * Deliberately SEPARATE from resistance: resistance says how much damage the
	 * medium does once it has hold, this says how readily it takes hold. A green
	 * tree wants a high value here and no damage resistance at all.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profile",
		meta = (ClampMin = "0.01"))
	float IgnitionScale = 1.f;

	/** Seconds afflicted once lit. Negative means "whatever the medium says". */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profile")
	float BurnSeconds = -1.f;

	/**
	 * Total energy this object feeds the fire across its whole burn.
	 *
	 * 0 -- the default -- means it burns without powering the fire onward: it
	 * takes damage, shows flames, and is a dead end. That is the right answer
	 * for anything not authored as real fuel, and it is what stops a random
	 * burnable prop becoming a forest-fire relay by accident.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profile",
		meta = (ClampMin = "0.0"))
	float EnergyYield = 0.f;
};

/**
 * One DIFFUSIVE spreadable medium -- fire, corruption, pestilence. Port of
 * Godot's SpreadDefinition.
 *
 * EVERY FIELD HERE IS ABOUT MOVEMENT THROUGH SPACE and nothing else. That
 * division is what keeps it small:
 *
 *   the status effect   what the medium DOES to a thing over time
 *   the target's stats  how much one particular thing cares
 *   this                how it MOVES between things
 *
 * The anchor is a MAGIC ELEMENT, and identity is read back off it -- "fire",
 * not "burning". One vocabulary spans spells, collisions and diffusion, so a
 * relationship authored once in the combination table means the same thing
 * everywhere it is consulted. A medium that is not castable still gets an
 * element holding little more than a tag and a status effect; that is cheap,
 * and it buys one vocabulary rather than two kept in step by hand.
 *
 * CONDUCTIVE media are deliberately NOT modelled here. Lightning arcing through
 * water is an instantaneous graph flood, not accumulating diffusion, and forcing
 * it into this tick loop would distort both -- see UARPGConductionSubsystem.
 */
UCLASS(BlueprintType)
class ARPGWORLD_API UARPGSpreadDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** What this medium IS. Its tag is the medium's identity everywhere. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	TObjectPtr<UARPGMagicElement> Element;

	// --- Propagation ----------------------------------------------------------

	/**
	 * Accumulated exposure a target must reach before the medium takes hold.
	 *
	 * Exposure builds and bleeds off, so a target briefly clipped by a fireball
	 * smoulders and recovers while one sitting in a firestorm catches.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Propagation",
		meta = (ClampMin = "0.01"))
	float IgnitionThreshold = 1.f;

	/** Beyond this an afflicted source contributes nothing, falling off linearly. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Propagation",
		meta = (ClampMin = "0.0"))
	float SpreadRadius = 400.f;

	/** Exposure per second cell-to-cell across open ground. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Propagation",
		meta = (ClampMin = "0.0"))
	float SpreadRate = 1.f;

	/**
	 * Exposure per second one afflicted OBJECT delivers, independent of
	 * SpreadRate. Negative means "same as SpreadRate".
	 *
	 * These need not match: how fast flame licks across grass and how hard a
	 * burning trunk radiates at its neighbours are different physical facts.
	 * Under a conserved budget the rate is only a SPEED -- range is bounded by
	 * energy either way.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Propagation")
	float ObjectSpreadRate = -1.f;

	/** Exposure per second shed by an unafflicted target receiving nothing. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Propagation",
		meta = (ClampMin = "0.0"))
	float DecayRate = 0.5f;

	/**
	 * How strongly wind skews propagation. 0 is a symmetric disc -- corruption
	 * creeping from a shrine -- and 1 is almost entirely downwind, a grass fire
	 * running before a gale.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Propagation",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WindBias = 0.5f;

	// --- Conserved energy -----------------------------------------------------

	/**
	 * Run this medium on a CONSERVED ENERGY BUDGET: every unit of exposure a
	 * burning cell delivers is PAID FOR out of a finite bank, seeded by whatever
	 * lit it and topped up by burning fuel. When the bank runs dry the thing
	 * keeps burning in place -- damage, status and visuals all continue -- but
	 * stops igniting anything else.
	 *
	 * THIS IS THE WHOLE CONTAINMENT STORY. It makes "how far does a fire travel"
	 * a linear function of "how much energy went in", instead of a binary
	 * stall-or-firestorm. With yields tuned below the cost of igniting fresh
	 * ground, a front spends the spell's deposit and dies at a range
	 * proportional to it; a forest only becomes self-sustaining when enough is
	 * alight at once that the combined yields cover the front's costs.
	 *
	 * Off is right for media whose reach should not depend on ignition strength
	 * -- corruption creeping from a shrine, a scripted hazard's fixed burn.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Energy")
	bool bConserveEnergy = true;

	/**
	 * Energy per second a burning cell feeds its own bank while it has fuel --
	 * what the grass contributes to carrying the fire onward.
	 *
	 * Igniting a fresh cell costs roughly IgnitionThreshold, so a total yield
	 * over the cell's fuel life BELOW that keeps open grass sub-critical: a pure
	 * grass fire always burns out, at a distance set by the energy that started
	 * it.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Energy",
		meta = (EditCondition = "bConserveEnergy", ClampMin = "0.0"))
	float FuelEnergyYield = 0.1f;

	/**
	 * Fraction of received exposure that lands in the receiver's own bank.
	 *
	 * The sender paid the FULL amount, so this gap is the front's per-hop range
	 * decay. Unlike a potency carry it can never amplify -- a cell can never
	 * hand on more than it was given.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Energy",
		meta = (EditCondition = "bConserveEnergy", ClampMin = "0.0", ClampMax = "1.0"))
	float TransferKeep = 0.5f;

	// --- Field ----------------------------------------------------------------

	/** Also spread through the ground field, not just object to object. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Field")
	bool bUsesField = true;

	/**
	 * Seconds of fuel a full-fuel cell holds. After this the cell is spent: it
	 * goes cold and cannot carry the medium again.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Field",
		meta = (EditCondition = "bUsesField", ClampMin = "0.1"))
	float FuelSeconds = 8.f;

	/** Never burns out, and ignores fuel entirely. Corruption, not fire. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Field")
	bool bPermanent = false;

	/**
	 * How much fuel each kind of ground is worth TO THIS MEDIUM, as a fraction of
	 * FuelSeconds.
	 *
	 * THE HALF THAT USED TO BE MISSING. The fuel map held one amount per cell,
	 * which is element-agnostic: a cell that carried fire well necessarily carried
	 * every medium well, differing only by FuelSeconds. So a marsh could not
	 * refuse fire and carry a frost, and stone could not be a firebreak that a
	 * corruption crossed happily.
	 *
	 * The ground says WHAT it is and the medium says what that is WORTH, which is
	 * the same split as everywhere else here -- an element pools because a fluid
	 * definition names it, not because the ground knows about water.
	 *
	 * A surface with no entry falls back to DefaultSurfaceFuel, so a table needs
	 * only the interesting cases: grass 1, stone 0, and let everything else be
	 * ordinary.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Field",
		meta = (EditCondition = "bUsesField"))
	TMap<TEnumAsByte<EPhysicalSurface>, float> SurfaceFuel;

	/**
	 * What an unlisted surface is worth. 1 keeps unbaked ground burning.
	 *
	 * Deliberately generous: a missing bake, a new surface type nobody has added
	 * to the table yet, and a test world all land here, and the failure mode of
	 * "burns when it should not" is far easier to notice than "silently
	 * fireproof".
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Field",
		meta = (EditCondition = "bUsesField", ClampMin = "0.0", ClampMax = "1.0"))
	float DefaultSurfaceFuel = 1.f;

	/** What one cell of this ground is worth to this medium, 0-1. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Spread")
	float GetSurfaceFuel(uint8 Surface) const;

	/** Contact damage per second to anything standing in a burning cell. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage",
		meta = (ClampMin = "0.0"))
	float ContactDamagePerSecond = 12.f;

	UFUNCTION(BlueprintPure, Category = "ARPG|Spread")
	float GetObjectSpreadRate() const
	{
		return ObjectSpreadRate >= 0.f ? ObjectSpreadRate : SpreadRate;
	}

	/** The medium's identity, read off the element. Invalid when unset. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Spread")
	FGameplayTag GetElementTag() const;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGSpread", GetFName());
	}

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
};
