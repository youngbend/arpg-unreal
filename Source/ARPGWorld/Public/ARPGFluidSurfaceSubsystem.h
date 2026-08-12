// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Subsystems/WorldSubsystem.h"
#include "ARPGFluidSurfaceSubsystem.generated.h"

class AARPGDischargeEffect;
class AARPGSurfaceBody;
class AARPGFluidPool;
class AARPGSolidBody;
class UARPGElementalVolumeComponent;
class UARPGFluidDefinition;
class UARPGMagicCombinationTable;
class UARPGSolidDefinition;

/**
 * Owns every persistent body of fluid and every solid frozen out of one. Port of
 * Godot's FluidSurfaceSystem.
 *
 * The division against the two systems it sits between is worth stating,
 * because all three are about "an element occupying part of the world":
 *
 *   SPREAD      a medium DIFFUSING through the ground -- a per-chunk cellular
 *               field, cheap, unbounded, invisible except as a shader mask.
 *   REACTION    two volumes MEETING -- instantaneous, local, resolved once.
 *   THIS        an element LYING somewhere -- a discrete body with an outline
 *               you can see, freeze, and stand on.
 *
 * A puddle is NOT a diffusion field with a mesh on it, and modelling it as one
 * was the thing to avoid: the field's cells are metres across and exist only
 * where something is already active, while "which part of this puddle does the
 * ice shard overlap" needs an exact region far below that scale.
 *
 * EVERY BODY IS A POLYGON, which is what reduces the whole system to four
 * library calls -- see ARPGFluidGeometry.
 *
 * NOTHING HERE KNOWS THAT WATER FREEZES. Which pairs solidify is a Solidify row
 * in the combination table's Surface scope, the same table and scoping every
 * other relationship uses. So ice + water making a floe and lava + water making
 * obsidian are two rows and two assets, and neither is a branch in this file.
 */
UCLASS()
class ARPGWORLD_API UARPGFluidSurfaceSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** What each fluid is like as a body. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Fluid")
	TArray<TObjectPtr<UARPGFluidDefinition>> Definitions;

	/** What each element is like frozen. A Solidify row with no entry warns. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Fluid")
	TArray<TObjectPtr<UARPGSolidDefinition>> Solids;

	/** The SAME table every other solver reads. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Fluid")
	TObjectPtr<UARPGMagicCombinationTable> CombinationTable;

	/**
	 * Rain, evaporation and melting ticks per second.
	 *
	 * These are slow physical processes and 4Hz is already finer than anyone can
	 * see. Every polygon rebuild in the system is charged to this rate, so it is
	 * the one number that decides what the whole thing costs when nothing is
	 * happening.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Fluid",
		meta = (ClampMin = "0.5", ClampMax = "30.0"))
	float TickRate = 4.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Fluid")
	bool bRaining = false;

	// --- Keeping the cost down -------------------------------------------------

	/**
	 * Beyond this from every viewer, a body stops paying for itself.
	 *
	 * WHAT STOPS AND WHAT DOES NOT. A distant body keeps SIMULATING -- it still
	 * melts and evaporates on the weather tick, because coming back to a pool that
	 * should have dried up an hour ago is a bug you cannot see happening. What
	 * stops is everything that exists for the player: the per-frame buoyancy
	 * settle, the mesh rebuild and, most of all, the collision cook.
	 *
	 * Generous by default. This is a safety rail against a hundred puddles across
	 * a field, not an LOD system.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Fluid",
		meta = (ClampMin = "0.0"))
	float SignificanceDistance = 8000.f;

	/**
	 * How many bodies of each kind the world will keep.
	 *
	 * NOTHING ELSE BOUNDS THIS. A deposit that merges into an existing pool is
	 * free, but one that lands clear of every pool spawns another actor -- so a
	 * player walking a field casting water makes one per cast, forever, each with
	 * a mesh, a collider and a replicated outline. Past the cap the smallest is
	 * retired, which is both the cheapest to lose and the least likely to be
	 * noticed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Fluid",
		meta = (ClampMin = "1"))
	int32 MaxBodiesOfEachKind = 48;

	/**
	 * Is anything close enough to this to be worth presenting?
	 *
	 * Asked by a body's own tick. Viewer positions are gathered once per weather
	 * tick rather than per body per frame, which is the whole point of asking the
	 * subsystem rather than each body walking the player list itself.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	bool IsSignificantAt(FVector WorldPosition) const;

	// --- Depositing -----------------------------------------------------------

	/**
	 * Leaves fluid on the ground. Merges into a nearby body of the same element
	 * rather than stacking a second one on top.
	 *
	 * @return the body the fluid ended up in, new or existing.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	AARPGFluidPool* Deposit(FVector WorldPosition, float Radius, FGameplayTag ElementTag);

	/** The capsule footprint of an elongated spell, rather than a disc. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	AARPGFluidPool* DepositSwept(FVector From, FVector To, float Radius, FGameplayTag ElementTag);

	/**
	 * How far below a finished spell this will look for ground to wet.
	 *
	 * A BODY LIES ON THE GROUND, and a spell finishes at chest height or in the
	 * air, so where it died is never where its puddle goes. Past this the spell
	 * expired over a drop or high overhead and wet nothing -- which is a real
	 * outcome and not a failure.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Fluid",
		meta = (ClampMin = "0.0"))
	float MaxDepositDrop = 1000.f;

	/**
	 * Puts a VOLUME of fluid back into the world, as opposed to wetting a radius.
	 *
	 * What melting a solid returns. The caller has a volume of stuff and no
	 * opinion about how wide the puddle should be, which is the fluid's own
	 * business: how far a given volume spreads is its Depth, and that is the only
	 * place in the system that turns one into the other.
	 *
	 * @return the body it ended up in, or null if it was too little to be one.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	AARPGFluidPool* ReturnFluid(FVector2D Where, float GroundHeight, double Volume,
		UARPGFluidDefinition* Definition);

	// --- Solidifying ----------------------------------------------------------

	/**
	 * Freezes the overlap of two volumes into a slab, when a Surface-scope
	 * Solidify row says that pair does so.
	 *
	 * Called by the reaction subsystem BEFORE it resolves an energy trade --
	 * this is the narrower question, and it either handles the meeting entirely
	 * or hands it straight back. Returns true when a slab was made.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	bool TrySolidify(UARPGElementalVolumeComponent* A, UARPGElementalVolumeComponent* B);

	// --- Queries --------------------------------------------------------------

	/** The pool whose polygon contains this point, or null. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	AARPGFluidPool* FindPoolAt(FVector WorldPosition) const;

	/**
	 * Is this point roofed over by a solid?
	 *
	 * NOT THROUGH THE ICE. A floe floating on a pool roofs over the water
	 * beneath it, and the pool's own collider knows nothing about that -- so a
	 * bolt that struck the ice would otherwise enter the water underneath and
	 * conduct from there. What it hit was the ice.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	bool IsCoveredBySolid(FVector WorldPosition) const;

	/**
	 * The slab standing at this point, or null.
	 *
	 * IsCoveredBySolid answers yes or no, which is all conduction needs. Anything
	 * that wants to DO something to the slab -- an earth spell picking up the wall
	 * in front of it and throwing it -- needs the slab.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	AARPGSolidBody* FindSolidAt(FVector WorldPosition) const;

	/**
	 * The nearest slab of a given element within a reach, or null.
	 *
	 * What a spell asks before deciding what it is. An earth Project cast at a
	 * wall the caster raised a moment ago throws the WALL rather than conjuring a
	 * boulder beside it, and this is the question that distinguishes the two --
	 * asked by the effect actor rather than by the ability, because the ability is
	 * shared by every element and only earth cares.
	 *
	 * @param ElementTag empty matches any element.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	AARPGSolidBody* FindSolidNear(FVector WorldPosition, float Reach,
		FGameplayTag ElementTag) const;

	// --- Slabs nothing solidified ----------------------------------------------

	/**
	 * Puts a slab this subsystem did not create into its register.
	 *
	 * FREEZING WAS THE ONLY WAY TO MAKE ONE, which quietly meant a slab was always
	 * something a fluid had turned into. An earth spell raising a wall out of the
	 * ground makes the same object for a completely different reason, and an
	 * unregistered one is a real actor that draws, collides and reacts -- but is
	 * invisible to IsCoveredBySolid, so a bolt would conduct through the water it
	 * is standing in as though the wall were not there.
	 *
	 * Idempotent, and the register holds no ownership: a slab destroyed by
	 * anything else drops out on the next sweep.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	void RegisterSolid(AARPGSolidBody* Solid);

	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	void UnregisterSolid(AARPGSolidBody* Solid);

	/** Every pool currently in the world. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	const TArray<AARPGFluidPool*>& GetPools() const { return Pools; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	const TArray<AARPGSolidBody*>& GetSolids() const { return ActiveSolids; }

	/** Runs one weather step immediately, bypassing the tick rate. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	void StepSimulation(float DeltaTime);

	/**
	 * Retires a body that has nothing left: drops it from the register and
	 * destroys it, and a melting solid returns its water on the way out.
	 *
	 * PUBLIC because a body reports its own exhaustion. A reaction that boils the
	 * last of a puddle away resolves inside the volume component, far from here,
	 * and the alternative -- letting the body call Destroy on itself -- leaves the
	 * subsystem holding a pointer to it until something notices.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	void RetireBody(AARPGSurfaceBody* Body);

	/**
	 * Tells whichever side is a slab WHERE a reaction touched it.
	 *
	 * The reaction hook reports how much energy was spent and nothing about where.
	 * For a puddle that is complete -- a liquid loses ground uniformly. For ice it
	 * is the whole behaviour, because melting at the point of impact is what makes
	 * a fireball cut an angle out of one edge instead of thinning the whole floe.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	void NoteReactionContact(UARPGElementalVolumeComponent* A, UARPGElementalVolumeComponent* B,
		FVector Contact);

	// --- Not paying for the same fluid twice -----------------------------------
	//
	// A REACTION HAS TWO WAYS TO PUT FLUID ON THE GROUND and they are easy to run
	// both at once. Melting a slab returns the material that melted, at the
	// contact, as it happens. Separately, the row's Result spawns a Collision
	// discharge, and a discharge that lands deposits whatever its element pools
	// as. For fire + ice -> water those are THE SAME WATER described twice, and
	// the puddle came out bigger than the ice that made it.
	//
	// The product's deposit is still right when no body was consumed -- two spells
	// meeting in mid-air to make water genuinely leave water and nothing else
	// accounted for it -- so the answer is not to switch it off, but to know
	// whether it was already covered.

	/** Starts a fresh reaction. Called by the reaction solver before it consumes. */
	void OpenReactionLedger();

	/** A body has handed this element's fluid back, so nothing else should. */
	void NoteFluidReturned(FGameplayTag ElementTag);

	/** Did a body already account for this element during the current reaction? */
	bool WasFluidReturned(FGameplayTag ElementTag) const;

private:
	UARPGFluidDefinition* FindDefinition(FGameplayTag ElementTag) const;
	UARPGSolidDefinition* FindSolidDefinition(FGameplayTag ElementTag) const;

	/** Merges a footprint into a nearby body, or spawns one. */
	AARPGFluidPool* DepositRing(const TArray<FVector2D>& Footprint, float GroundHeight,
		UARPGFluidDefinition* Definition);

	/**
	 * A cast spell leaving its element on the ground. THE ONLY THING THAT PUTS A
	 * PUDDLE IN THE WORLD from gameplay -- everything else here grows, erodes,
	 * freezes or melts one that already exists.
	 */
	void HandleDischargeLanded(AARPGDischargeEffect* Effect);

	/**
	 * Drops a world position onto the ground beneath it. False when there is none.
	 *
	 * @param Ignore the spell itself, which is still in the world at the moment it
	 *        finishes and whose own collider would otherwise be what it lands on.
	 */
	bool TraceToGround(FVector From, const AActor* Ignore, FVector& OutGround) const;

	void TickWeather(float DeltaTime);

	/** True where this machine owns the simulation; pools are server-spawned. */
	bool HasAuthority() const;

	UPROPERTY(Transient)
	TArray<AARPGFluidPool*> Pools;

	UPROPERTY(Transient)
	TArray<AARPGSolidBody*> ActiveSolids;

	FDelegateHandle DischargeLandedHandle;

	/** So "nothing is configured to pool" is said once rather than every cast. */
	mutable bool bWarnedNoDefinitions = false;

	/** Destroys everything floating on a pool that is about to stop existing. */
	void DropRiders(AARPGFluidPool* Pool);

	/** Refreshes ViewerLocations. Once per weather tick, not once per body. */
	void GatherViewers();

	/** Drops the smallest bodies until the register is back inside its cap. */
	void EnforceBudget();

	TArray<FVector> ViewerLocations;

	/**
	 * Elements a body has handed back during the reaction being resolved.
	 *
	 * Scoped to one Resolve rather than timed: the solver opens the ledger, the
	 * Consume calls fill it, and the product is spawned before anything else runs.
	 * A window in seconds would have to guess how long a discharge lives.
	 */
	TArray<FGameplayTag> FluidReturnedThisReaction;

	float TickAccumulator = 0.f;
};
