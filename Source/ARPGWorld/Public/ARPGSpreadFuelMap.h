// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ARPGSpreadFuelMap.generated.h"

/** One chunk's worth of baked fuel, row-major by Y then X. */
USTRUCT()
struct ARPGWORLD_API FARPGFuelChunk
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<uint8> Cells;
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
	 * Fuel fraction 0-1 at a normalised position within a chunk.
	 *
	 * Returns 1 for a chunk with no entry -- see the class comment: unbaked
	 * ground burns, rather than a missing bake making the world fireproof.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Spread")
	float GetFuelAt(FIntPoint ChunkCoord, float NormalisedX, float NormalisedY) const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Spread")
	bool HasChunk(FIntPoint ChunkCoord) const { return Chunks.Contains(ChunkCoord); }

	/** Writes one chunk's cells. Used by the bake commandlet. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Spread")
	void SetChunkCells(FIntPoint ChunkCoord, const TArray<uint8>& Cells);

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGFuelMap", GetFName());
	}
};
