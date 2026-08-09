// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGSpreadDefinition.h"
#include "ARPGMagicElement.h"
#include "ARPGSpreadFuelMap.h"
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

float UARPGSpreadFuelMap::GetFuelAt(FIntPoint ChunkCoord, float NormalisedX, float NormalisedY) const
{
	const FARPGFuelChunk* Chunk = Chunks.Find(ChunkCoord);
	if (!Chunk || Chunk->Cells.Num() == 0)
	{
		// Unbaked ground burns. See the class comment: a missing chunk means
		// "never baked", not "no fuel", and treating the two the same would make
		// an unbaked test world silently fireproof.
		return 1.f;
	}

	const int32 X = FMath::Clamp(FMath::FloorToInt(NormalisedX * Resolution), 0, Resolution - 1);
	const int32 Y = FMath::Clamp(FMath::FloorToInt(NormalisedY * Resolution), 0, Resolution - 1);

	const int32 Index = Y * Resolution + X;
	return Chunk->Cells.IsValidIndex(Index) ? Chunk->Cells[Index] / 255.f : 1.f;
}

void UARPGSpreadFuelMap::SetChunkCells(FIntPoint ChunkCoord, const TArray<uint8>& Cells)
{
	FARPGFuelChunk& Chunk = Chunks.FindOrAdd(ChunkCoord);
	Chunk.Cells = Cells;
}
