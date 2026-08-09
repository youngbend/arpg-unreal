// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ARPGMasteryWindow.h"
#include "ARPGProgressionTypes.h"
#include "ARPGProgressionTrackerComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FARPGOnCategoryLevelUp,
	int32, NewLevel, float, TotalXP);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FARPGOnSubcomponentLevelUp,
	FGameplayTag, Subcomponent, int32, NewLevel);

/**
 * Hidden progression for one category -- magic, weapon or armour. Port of the
 * three Godot tracker nodes, which were one design written three times.
 *
 * WHY ONE BASE CLASS. The Godot trackers differed only in what they listened to
 * and what they called a subcomponent; the level model, the mastery window, the
 * effective-level query and the multiplier were duplicated across all three.
 * Keying every category by gameplay tag -- Element.Fire, Weapon.Sword,
 * Armor.Heavy are all just "which thing did you use" -- collapses that to one
 * implementation plus three small subclasses that only wire up their XP source.
 *
 * TWO LEVELS, NOT ONE. Category level rises with total use and gives a step
 * bonus; subcomponent proficiency rises 0-3 per element or type and then hands
 * over to mastery, a rolling window of recent use that FADES. So a returning
 * player keeps everything they earned and loses only the sharpness of what they
 * have not touched lately -- which is the whole reason mastery is a window query
 * rather than a fourth stored level.
 *
 * HIDDEN, in the sense that the player never allocates any of it: it accrues
 * from doing the thing. The visible, allocatable level is
 * UARPGProgressionComponent.
 */
UCLASS(Abstract, ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCORE_API UARPGProgressionTrackerComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGProgressionTrackerComponent();

	virtual void BeginPlay() override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Progression")
	TObjectPtr<UARPGProgressionSettings> Settings;

	/**
	 * Reports every subcomponent at the maximum effective level.
	 *
	 * A testing aid with real teeth: it bypasses the magic complexity gate AND
	 * maxes every damage multiplier at once, so trying a combination does not
	 * mean grinding both its elements to level 3 first.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Progression")
	bool bDebugForceMaxLevel = false;

	// --- Queries --------------------------------------------------------------

	/** Continuous 0-4 level for one subcomponent. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Progression")
	float GetEffectiveLevel(FGameplayTag Subcomponent) const;

	/** The shared multiplier for one subcomponent -- see UARPGProgressionSettings. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Progression")
	float GetMultiplier(FGameplayTag Subcomponent) const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Progression")
	int32 GetCategoryLevel() const { return Category.Level; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Progression")
	float GetCategoryXP() const { return Category.TotalXP; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Progression")
	int32 GetSubcomponentLevel(FGameplayTag Subcomponent) const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Progression")
	float GetMasteryFraction(FGameplayTag Subcomponent) const;

	// --- Recording ------------------------------------------------------------

	/**
	 * One use of a subcomponent: grants XP to both the category and the
	 * subcomponent, and records the event in the mastery window.
	 *
	 * The window is recorded even when the XP is zero, because using something
	 * is what mastery measures -- a maxed element still has to be USED to stay
	 * masterful.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Progression")
	void RecordUse(FGameplayTag Subcomponent, float XP);

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Progression")
	FARPGOnCategoryLevelUp OnCategoryLevelUp;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Progression")
	FARPGOnSubcomponentLevelUp OnSubcomponentLevelUp;

	// --- Save / load ----------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "ARPG|Progression")
	FARPGCategoryProgress GetCategoryProgress() const { return Category; }

	void RestoreProgress(const FARPGCategoryProgress& InCategory,
		const TMap<FGameplayTag, FARPGSubcomponentProgress>& InSubcomponents,
		const TArray<FGameplayTag>& MasteryHistory);

	const TMap<FGameplayTag, FARPGSubcomponentProgress>& GetSubcomponents() const { return Subcomponents; }
	const TArray<FGameplayTag>& GetMasteryHistory() const { return MasteryWindow.GetHistory(); }

protected:
	/** Subclasses bind their XP source here, once the owner's components exist. */
	virtual void BindXPSource() {}

	UPROPERTY()
	FARPGCategoryProgress Category;

	UPROPERTY()
	TMap<FGameplayTag, FARPGSubcomponentProgress> Subcomponents;

	FARPGMasteryWindow MasteryWindow;
};
