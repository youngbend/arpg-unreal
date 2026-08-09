// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "ARPGProgressionTypes.generated.h"

class UARPGXPCurve;

/**
 * Level and XP for one hidden progression CATEGORY -- magic, weapon or armour
 * as a whole. Port of Godot's CategoryProgress.
 *
 * A struct rather than an object: it is two numbers with no identity, and being
 * a struct is what lets a tracker replicate it as one field.
 */
USTRUCT(BlueprintType)
struct ARPGCORE_API FARPGCategoryProgress
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Progression")
	int32 Level = 1;

	UPROPERTY(BlueprintReadOnly, Category = "Progression")
	float TotalXP = 0.f;

	/** Adds XP and re-solves the level. Returns true when the level went up. */
	bool AddXP(float Amount, const UARPGXPCurve* Curve);

	/**
	 * The category's step-function bonus: floor(Level / TierSize) * BonusPerTier.
	 *
	 * A step rather than a smooth curve so the player feels a category level as
	 * an event -- a continuous trickle is indistinguishable from nothing.
	 */
	float GetStepBonus(int32 TierSize, float BonusPerTier) const;
};

/**
 * Proficiency with one SUBCOMPONENT -- a single element, weapon type or armour
 * type -- within a category. Port of Godot's SubcomponentProgress.
 *
 * THE 0-3 PART IS STORED HERE AND THE 3-4 PART IS NOT. Base level is XP-driven
 * at a flat cost per level. The continuous "mastery" tail above 3 is a
 * rolling-window usage fraction, supplied by the tracker's shared ring buffer at
 * query time -- so mastery decays when you stop using something, which a stored
 * number could not express without a decay tick.
 */
USTRUCT(BlueprintType)
struct ARPGCORE_API FARPGSubcomponentProgress
{
	GENERATED_BODY()

	/** 0-3. Above that, proficiency is maxed and mastery takes over. */
	UPROPERTY(BlueprintReadOnly, Category = "Progression")
	int32 BaseLevel = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Progression")
	float XPIntoLevel = 0.f;

	/** Adds XP toward the next 0-3 level. No-op once maxed. Returns true on level-up. */
	bool AddXP(float Amount, float XPPerLevel);

	/** Continuous 0-3: the level plus fractional progress into the next. */
	float GetProficiencyPartial(float XPPerLevel) const;

	/**
	 * Continuous 0-4 effective level. Below 3 this is the XP-driven partial;
	 * at 3 it becomes 3 + MasteryFraction, and never falls back below 3.
	 *
	 * The floor matters: proficiency is EARNED and permanent, mastery is
	 * CURRENT and fades. Letting a decaying window drag the effective level
	 * below 3 would silently re-lock combinations the player had unlocked.
	 */
	float GetEffectiveLevel(float MasteryFraction, float XPPerLevel) const;
};

/**
 * Tunables for one progression category. Port of Godot's ProgressionSettings.
 *
 * Author a separate asset per category so magic, weapon and armour weights tune
 * independently.
 */
UCLASS(BlueprintType)
class ARPGCORE_API UARPGProgressionSettings : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Category")
	TObjectPtr<UARPGXPCurve> CategoryXPCurve;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Category")
	float StepBonusPerTier = 0.05f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Category",
		meta = (ClampMin = "1"))
	int32 StepTierSize = 5;

	/** Flat, not polynomial: each of the 0-3 proficiency levels costs this. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Subcomponent",
		meta = (ClampMin = "0.01"))
	float SubcomponentXPPerLevel = 500.f;

	/** Rolling window size -- "the last N casts or hits" -- for mastery. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Subcomponent",
		meta = (ClampMin = "1"))
	int32 MasteryWindow = 800;

	/**
	 * Flat XP for landing a status from an element that deals no damage.
	 *
	 * Needed because the usual XP source is proportional to damage, and a
	 * purely debuffing element would otherwise never progress at all.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Subcomponent",
		meta = (ClampMin = "0.0"))
	float StatusApplicationXP = 100.f;

	// --- Multiplier weights ---------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weights")
	float CategoryWeight = 1.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weights")
	float ProficiencyWeight = 1.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weights")
	float MasteryWeight = 1.f;

	/**
	 * The one multiplier all three trackers share:
	 *
	 *   (1 + w1*step) * (1 + w2*prof) * (1 + w3*mastery)
	 *
	 * where prof = min(effective, 3)/3 and mastery = max(0, effective - 3).
	 *
	 * MULTIPLICATIVE, not additive, so the three axes stay independent: a player
	 * deep in one category and shallow in an element still gets the category's
	 * benefit, and no term can cancel another out. Static because the armour
	 * tracker applies it to mitigation while the other two apply it to outgoing
	 * damage -- same formula, three consumers, one place to change it.
	 *
	 * Null settings fall back to the documented defaults, so a tracker with
	 * nothing authored still behaves sanely rather than multiplying by zero.
	 */
	static float ComputeMultiplier(const UARPGProgressionSettings* Settings,
		const FARPGCategoryProgress& Category, float EffectiveLevel);
};
