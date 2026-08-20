// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ARPGSpreadFuelMap.generated.h"

/** One chunk's worth of baked ground, row-major by Y then X. */
USTRUCT()
struct ARPGWORLD_API FARPGFuelChunk
{
	GENERATED_BODY()

	/**
	 * WHAT KIND OF GROUND this cell is, as an EPhysicalSurface value.
	 *
	 * NOT HOW MUCH FUEL, which is what it used to be and was the wrong question.
	 * A single amount is element-agnostic: a cell that carries fire well
	 * necessarily carried every medium well, differing only by a per-medium
	 * constant, so a marsh could not both refuse fire and carry a frost.
	 *
	 * The surface is a fact about the world and how much fuel it is worth is a
	 * fact about the medium, and separating them costs nothing -- the same byte
	 * per cell, and each spread definition brings its own table.
	 *
	 * AND THE ENGINE ALREADY ANSWERS THIS EVERYWHERE. Landscape layers carry a
	 * physical material, so do meshes, and a trace returns one. The bake becomes
	 * "rasterise the dominant surface per cell", which is both cheaper than
	 * sampling foliage density and something a level designer already authors for
	 * footstep sounds.
	 */
	UPROPERTY()
	TArray<uint8> Cells;

	/**
	 * Per-cell variation, 0-255, mapping to a multiplier around 1.
	 *
	 * BECAUSE A UNIFORM GRID LOOKS LIKE A GRID. The Godot version's map was
	 * regular, and a regular map burns in diamonds: every cell in a ring reaches
	 * ignition on the same tick, so the fire front is a shape the grid chose
	 * rather than a shape the fire did. Worse, it made the same fire look
	 * different depending on where the viewer stood relative to a cell boundary,
	 * which reads as the simulation being unreliable rather than as aliasing.
	 *
	 * Jitter breaks the tie. Cells reach ignition at slightly different times, so
	 * the front is ragged and no two runs of the same fire trace the same
	 * diamond. Baked rather than hashed at runtime, so a given patch of ground
	 * always burns the same way -- ragged, not random.
	 *
	 * Empty means no variation, which is what an old bake and every test gets.
	 */
	UPROPERTY()
	TArray<uint8> Jitter;
};

/**
 * How much fuel the ground actually holds, per field cell, baked offline. Port
 * of Godot's SpreadFuelMap.
 *
 * WHY BAKED RATHER THAN SAMPLED AT RUNTIME. The source is landscape foliage
 * density, which is expensive to query per cell and does not change during
 * play. Baking it makes "bare dirt, rock and river beds cannot carry a fire" a
 * property of the WORLD rather than something the simulation has to rediscover
 * every tick -- and a firebreak becomes something a level designer paints.
 *
 * A chunk with NO entry falls back to uniform FULL fuel. The bake writes an
 * entry for every landscape region, all-zero ones included, so a missing chunk
 * means ground that was never part of the baked world at all: tools, tests and
 * unbaked projects keep the "everything burns" behaviour there rather than
 * silently becoming fireproof.
 */
UCLASS(BlueprintType)
class ARPGWORLD_API UARPGSpreadFuelMap : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/**
	 * Cells per chunk edge in THIS map.
	 *
	 * Need not equal the simulation's own field resolution -- lookups are by
	 * normalised position -- but matching it wastes nothing.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fuel",
		meta = (ClampMin = "1", ClampMax = "1024"))
	int32 Resolution = 64;

	/**
	 * Chunk edge in centimetres this map was baked against.
	 *
	 * The subsystem warns when it disagrees with its own chunk size, because a
	 * grid mismatch silently SHEARS every lookup -- fuel from one place applied
	 * to another, with nothing visibly wrong.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fuel",
		meta = (ClampMin = "1.0"))
	float ChunkSize = 15360.f;

	UPROPERTY()
	TMap<FIntPoint, FARPGFuelChunk> Chunks;

	/**
	 * What kind of ground is at a normalised position within a chunk.
	 *
	 * Returns SurfaceType_Default for a chunk with no entry -- see the class
	 * comment: unbaked ground burns, rather than a missing bake making the world
	 * fireproof, and Default is what a definition's table falls back to.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Spread")
	uint8 GetSurfaceAt(FIntPoint ChunkCoord, float NormalisedX, float NormalisedY) const;

	/**
	 * How much this cell varies from its surface's nominal fuel, as a multiplier.
	 *
	 * 1 exactly where nothing was baked, so an unjittered map behaves as it
	 * always did and a test does not have to reason about noise.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Spread")
	float GetJitterAt(FIntPoint ChunkCoord, float NormalisedX, float NormalisedY) const;

	/**
	 * How far either side of nominal the jitter reaches, 0-1.
	 *
	 * 0.35 means a cell holds between 65% and 135% of what its surface says. Big
	 * enough that a fire front is visibly ragged, small enough that a firebreak
	 * is still a firebreak.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fuel",
		meta = (ClampMin = "0.0", ClampMax = "0.9"))
	float JitterRange = 0.35f;

	UFUNCTION(BlueprintPure, Category = "ARPG|Spread")
	bool HasChunk(FIntPoint ChunkCoord) const { return Chunks.Contains(ChunkCoord); }

	/**
	 * Writes one chunk's surfaces, and jitters them. Used by the bake commandlet.
	 *
	 * JITTERED HERE rather than by the baker, so every map gets it and no bake
	 * has to remember. Deterministic from the cell's world coordinate, so the
	 * same ground is always the same, and two bakes of one level agree.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Spread")
	void SetChunkCells(FIntPoint ChunkCoord, const TArray<uint8>& Cells);

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGFuelMap", GetFName());
	}
};
