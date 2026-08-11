// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Subsystems/WorldSubsystem.h"
#include "ARPGFluidSurfaceSubsystem.generated.h"

class AARPGDischargeEffect;
class AARPGFluidBody;
class AARPGFluidPool;
class AARPGFluidSolid;
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

	/** Every pool currently in the world. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	const TArray<AARPGFluidPool*>& GetPools() const { return Pools; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	const TArray<AARPGFluidSolid*>& GetSolids() const { return ActiveSolids; }

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
	void RetireBody(AARPGFluidBody* Body);

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
	TArray<AARPGFluidSolid*> ActiveSolids;

	FDelegateHandle DischargeLandedHandle;

	/** So "nothing is configured to pool" is said once rather than every cast. */
	mutable bool bWarnedNoDefinitions = false;

	float TickAccumulator = 0.f;
};
