// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "ARPGMasteryWindow.generated.h"

/**
 * A fixed-capacity rolling window of "the last N events, and what each was
 * attributed to". Port of Godot's MasteryRingBuffer.
 *
 * ONE WINDOW PER CATEGORY, SHARED BY EVERY SUBCOMPONENT IN IT. Mastery with fire
 * is not "how much fire have you cast" but "what fraction of your recent casting
 * was fire" -- so it is inherently a query over a shared history rather than a
 * per-element counter. That is what makes mastery a CURRENT skill that fades
 * when you switch elements, rather than another number that only goes up.
 *
 * The id-to-count map is maintained incrementally so GetFraction is O(1). It is
 * queried on every cast and every hit for the damage multiplier, which is why an
 * O(N) scan of an 800-entry window is not acceptable.
 */
USTRUCT(BlueprintType)
struct ARPGCORE_API FARPGMasteryWindow
{
	GENERATED_BODY()

	/** Resizing rebuilds the window, keeping the most recent events that fit. */
	void SetCapacity(int32 NewCapacity);

	int32 GetCapacity() const { return Capacity; }

	/** Records one event, evicting the oldest once full. */
	void Record(FGameplayTag SubcomponentTag);

	/** Fraction of the current window attributed to this tag. 0 when empty. */
	float GetFraction(FGameplayTag SubcomponentTag) const;

	int32 GetEventCount() const { return Events.Num(); }

	/** The raw history, for save/load. */
	const TArray<FGameplayTag>& GetHistory() const { return Events; }
	void SetHistory(const TArray<FGameplayTag>& History);

private:
	void RebuildCounts();

	UPROPERTY()
	int32 Capacity = 800;

	/**
	 * A plain array used as a ring, not a TQueue: mastery has to serialise for
	 * save/load, and the whole history in order is the thing worth saving.
	 */
	UPROPERTY()
	TArray<FGameplayTag> Events;

	UPROPERTY()
	int32 WriteIndex = 0;

	UPROPERTY()
	TMap<FGameplayTag, int32> Counts;
};
