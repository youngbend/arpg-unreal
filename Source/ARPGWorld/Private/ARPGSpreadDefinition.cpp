// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGSpreadDefinition.h"
#include "ARPGMagicElement.h"
#include "ARPGSpreadFuelMap.h"
#include "Chaos/ChaosEngineInterface.h"
#include "GameplayEffect.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

FGameplayTag UARPGSpreadDefinition::GetElementTag() const
{
	return Element ? Element->ElementTag : FGameplayTag();
}

#if WITH_EDITOR
EDataValidationResult UARPGSpreadDefinition::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	if (!Element)
	{
		Context.AddError(FText::FromString(
			TEXT("No Element set. The element IS this medium's identity -- without it the "
			     "medium cannot be registered, matched in a combination row, or applied.")));
		return EDataValidationResult::Invalid;
	}

	if (!Element->ElementTag.IsValid())
	{
		Context.AddError(FText::FromString(
			TEXT("The Element has no tag, so this medium has no identity.")));
		Result = EDataValidationResult::Invalid;
	}

	if (!Element->OnHitEffect)
	{
		Context.AddWarning(FText::FromString(
			TEXT("The Element has no OnHitEffect, so nothing this medium takes hold of will "
			     "actually carry a status -- it will spread invisibly and do nothing.")));
	}

	// The sub-critical condition, stated as a check rather than left to whoever
	// tunes it: total yield over a cell's life below the cost of lighting the
	// next cell is what makes open grass burn out instead of consuming the map.
	if (bConserveEnergy && !bPermanent)
	{
		const float TotalYield = FuelEnergyYield * FuelSeconds;
		if (TotalYield >= IgnitionThreshold)
		{
			Context.AddWarning(FText::FromString(FString::Printf(
				TEXT("A cell yields %.2f over its life but igniting one costs %.2f, so open "
				     "ground is SELF-SUSTAINING: any fire will consume every connected cell "
				     "with fuel. Intended for a firestorm; otherwise lower FuelEnergyYield."),
				TotalYield, IgnitionThreshold)));
		}
	}

	return Result;
}
#endif

// ---------------------------------------------------------------------------
// Fuel map
// ---------------------------------------------------------------------------

float UARPGSpreadDefinition::GetSurfaceFuel(uint8 Surface) const
{
	// A TABLE WITH ONLY THE INTERESTING CASES. Grass 1, stone 0, and everything
	// else ordinary -- a medium should not have to enumerate every surface in the
	// project to say that it burns grass.
	if (const float* Found = SurfaceFuel.Find(static_cast<EPhysicalSurface>(Surface)))
	{
		return FMath::Clamp(*Found, 0.f, 1.f);
	}

	return FMath::Clamp(DefaultSurfaceFuel, 0.f, 1.f);
}

namespace
{
	/** Cell index for a normalised position, or INDEX_NONE off the grid. */
	int32 CellFor(int32 Resolution, float NormalisedX, float NormalisedY)
	{
		if (Resolution <= 0)
		{
			return INDEX_NONE;
		}

		const int32 X = FMath::Clamp(FMath::FloorToInt(NormalisedX * Resolution), 0, Resolution - 1);
		const int32 Y = FMath::Clamp(FMath::FloorToInt(NormalisedY * Resolution), 0, Resolution - 1);

		return Y * Resolution + X;
	}
}

uint8 UARPGSpreadFuelMap::GetSurfaceAt(FIntPoint ChunkCoord, float NormalisedX,
	float NormalisedY) const
{
	const FARPGFuelChunk* Chunk = Chunks.Find(ChunkCoord);
	if (!Chunk || Chunk->Cells.Num() == 0)
	{
		// Unbaked ground burns. A missing chunk means "never baked", not "no
		// fuel", and treating the two the same would make an unbaked test world
		// silently fireproof. Default is what a definition's table falls back to.
		return static_cast<uint8>(SurfaceType_Default);
	}

	const int32 Index = CellFor(Resolution, NormalisedX, NormalisedY);

	return Chunk->Cells.IsValidIndex(Index)
		? Chunk->Cells[Index]
		: static_cast<uint8>(SurfaceType_Default);
}

float UARPGSpreadFuelMap::GetJitterAt(FIntPoint ChunkCoord, float NormalisedX,
	float NormalisedY) const
{
	const FARPGFuelChunk* Chunk = Chunks.Find(ChunkCoord);
	const int32 Index = CellFor(Resolution, NormalisedX, NormalisedY);

	// EXACTLY ONE where nothing was baked, so an old map behaves as it always did
	// and a test does not have to reason about noise it did not ask for.
	if (!Chunk || !Chunk->Jitter.IsValidIndex(Index) || JitterRange <= 0.f)
	{
		return 1.f;
	}

	// 0-255 onto [1 - Range, 1 + Range].
	const float Signed = (Chunk->Jitter[Index] / 255.f) * 2.f - 1.f;
	return FMath::Max(0.f, 1.f + Signed * JitterRange);
}

void UARPGSpreadFuelMap::SetChunkCells(FIntPoint ChunkCoord, const TArray<uint8>& Cells)
{
	FARPGFuelChunk& Chunk = Chunks.FindOrAdd(ChunkCoord);
	Chunk.Cells = Cells;

	// JITTERED HERE, so every bake gets it and no baker has to remember. A
	// uniform grid burns in diamonds -- every cell in a ring reaches ignition on
	// the same tick, so the front is a shape the grid chose rather than one the
	// fire did -- and that regularity is what made the Godot version's fires look
	// different depending on where the viewer stood relative to a cell boundary.
	//
	// HASHED FROM THE CELL'S WORLD COORDINATE rather than from a random stream, so
	// a given patch of ground always burns the same way and two bakes of one
	// level agree. Ragged, not random.
	Chunk.Jitter.SetNumUninitialized(Cells.Num());

	for (int32 Index = 0; Index < Cells.Num(); ++Index)
	{
		const int32 X = Resolution > 0 ? Index % Resolution : 0;
		const int32 Y = Resolution > 0 ? Index / Resolution : 0;

		const uint32 Hash = HashCombine(
			HashCombine(GetTypeHash(ChunkCoord.X), GetTypeHash(ChunkCoord.Y)),
			HashCombine(GetTypeHash(X), GetTypeHash(Y)));

		Chunk.Jitter[Index] = static_cast<uint8>(Hash & 0xFF);
	}
}
