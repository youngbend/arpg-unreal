// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ARPGXPCurve.generated.h"

/**
 * A polynomial XP curve. Port of Godot's XPCurve.
 *
 *   XpToNext(level) = Base * level ^ Exponent
 *
 * Shared by the visible point-allocation level and by each hidden category
 * level (magic, weapon, armour) -- author a separate asset per curve so they
 * tune independently.
 *
 * A UDataAsset rather than a UCurveFloat because the shape is genuinely two
 * numbers: a curve asset would invite hand-drawn per-level values that then
 * have to be maintained past the level cap.
 */
UCLASS(BlueprintType)
class ARPGCORE_API UARPGXPCurve : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Curve",
		meta = (ClampMin = "0.01"))
	float Base = 100.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Curve",
		meta = (ClampMin = "0.1", ClampMax = "10.0"))
	float Exponent = 1.5f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Curve",
		meta = (ClampMin = "1"))
	int32 LevelCap = 50;

	/**
	 * XP to advance from this level to the next.
	 *
	 * Returns effectively infinity at the cap, so callers need no separate
	 * "are we capped" branch -- the loop that spends XP simply never clears it.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Progression")
	float GetXPToNext(int32 Level) const;

	/**
	 * The level cumulative XP buys, and how much is left over into it.
	 *
	 * Remainder is 0 at the cap: there is no "into the next level" when there
	 * is no next level, and reporting leftovers would make a capped bar look
	 * partly full forever.
	 */
	void SolveLevel(float TotalXP, int32& OutLevel, float& OutRemainder) const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Progression")
	int32 GetLevelForTotalXP(float TotalXP) const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Progression")
	float GetRemainderForTotalXP(float TotalXP) const;
};
