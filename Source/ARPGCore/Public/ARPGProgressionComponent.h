// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ARPGProgressionComponent.generated.h"

class UAbilitySystemComponent;
class UARPGXPCurve;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FARPGOnXPGained, float, Amount, float, TotalXP);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FARPGOnLevelUp, int32, NewLevel, int32, PointsGranted);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnPointsChanged, int32, AvailablePoints);

/** Which pool a level-up point was spent into. */
UENUM(BlueprintType)
enum class EARPGAttributePoint : uint8
{
	Health,
	Stamina,
	Mana
};

/**
 * The VISIBLE level: XP, level-ups, and points the player allocates by hand.
 * Port of Godot's ProgressionComponent.
 *
 * The counterpart to UARPGProgressionTrackerComponent, which is the hidden half.
 * The split is the design: what you get better AT accrues invisibly from doing
 * it, while what you become is spent deliberately. Neither reads the other.
 *
 * ALLOCATION RECOMPUTES AN ABSOLUTE TOTAL rather than adding a delta:
 *
 *     MaxHealth = BaseMaxHealth + AllocatedHealth * HealthPerPoint
 *
 * so re-applying a save is idempotent. A delta-based version double-counts every
 * point every time the game is loaded, which is invisible in a single session
 * and unbounded across many.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCORE_API UARPGProgressionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGProgressionComponent();

	virtual void BeginPlay() override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Progression")
	TObjectPtr<UARPGXPCurve> XPCurve;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Progression",
		meta = (ClampMin = "0"))
	int32 PointsPerLevel = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Progression",
		meta = (ClampMin = "0.0"))
	float HealthPerPoint = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Progression",
		meta = (ClampMin = "0.0"))
	float StaminaPerPoint = 5.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Progression",
		meta = (ClampMin = "0.0"))
	float ManaPerPoint = 5.f;

	// --- Queries --------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "ARPG|Progression")
	int32 GetLevel() const { return Level; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Progression")
	float GetTotalXP() const { return TotalXP; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Progression")
	float GetXPIntoLevel() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Progression")
	float GetXPToNextLevel() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Progression")
	int32 GetAvailablePoints() const { return AvailablePoints; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Progression")
	int32 GetAllocatedPoints(EARPGAttributePoint Which) const;

	// --- Mutation -------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "ARPG|Progression")
	void GrantXP(float Amount);

	/** Returns false when there is no point to spend. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Progression")
	bool SpendPoint(EARPGAttributePoint Which);

	// --- Save / load ----------------------------------------------------------

	/**
	 * Restores a saved state in one call, then reapplies allocations.
	 *
	 * One call rather than separate setters because the three are only coherent
	 * together: a level without its XP, or allocations without the points that
	 * paid for them, is a state the player could never have reached.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Progression")
	void RestoreProgress(int32 InLevel, float InTotalXP, int32 InAvailablePoints,
		int32 InHealthPoints, int32 InStaminaPoints, int32 InManaPoints);

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Progression")
	FARPGOnXPGained OnXPGained;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Progression")
	FARPGOnLevelUp OnLevelUp;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Progression")
	FARPGOnPointsChanged OnPointsChanged;

private:
	UAbilitySystemComponent* GetASC() const;

	/**
	 * Records the character's unallocated maxima, once.
	 *
	 * Captured lazily rather than in BeginPlay: for a player the ability system
	 * lives on the PlayerState, which may not exist yet -- and capturing zeroes
	 * would permanently reset the character's maxima to whatever points they had
	 * spent.
	 */
	void CaptureBaseValues();

	/** Writes base + allocated*per-point to each max attribute. */
	void ReapplyAllocations();

	int32& AllocationFor(EARPGAttributePoint Which);

	UPROPERTY(Transient)
	mutable TObjectPtr<UAbilitySystemComponent> CachedASC;

	int32 Level = 1;
	float TotalXP = 0.f;
	int32 AvailablePoints = 0;

	int32 AllocatedHealth = 0;
	int32 AllocatedStamina = 0;
	int32 AllocatedMana = 0;

	float BaseMaxHealth = 0.f;
	float BaseMaxStamina = 0.f;
	float BaseMaxMana = 0.f;
	bool bCapturedBase = false;
};
