// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ARPGConsumableDefinition.generated.h"

class UAnimMontage;
class UGameplayEffect;

/**
 * A potion, food, scroll. Port of Godot's ConsumableDefinition.
 *
 * THE EFFECT TYPE ENUM IS GONE. Godot's ConsumableEffect carried a ten-value
 * enum -- restore health, restore over time, buff damage amp, buff resistance,
 * grant status immunity -- with a switch that applied each. Every one of those
 * is precisely what a GameplayEffect is:
 *
 *   restore health          instant effect on the Health attribute
 *   restore over time       duration effect with a period
 *   buff damage amp         duration effect on DamageAmpMultiplier
 *   buff resistance         duration effect on a resistance attribute
 *   grant status immunity   GrantedApplicationImmunityTags
 *
 * Keeping the enum would mean a second, worse effect system living inside a
 * project that already has GAS -- the same mistake as a parallel damage path.
 * The cost is that "restore 50 health" is an asset rather than a number; the
 * gain is that consumables stack, refresh, get dispelled and get resisted for
 * free, and a designer can express one that nothing in the enum could.
 */
UCLASS(BlueprintType)
class ARPGCOMBAT_API UARPGConsumableDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FName ConsumableId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FText DisplayName;

	/** Applied in order when the item is consumed. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Effects")
	TArray<TSubclassOf<UGameplayEffect>> Effects;

	/**
	 * Seconds before this can be used again.
	 *
	 * Per DEFINITION rather than per item, so every healing potion shares one
	 * cooldown and chugging three different ones is a deliberate choice the
	 * design allows rather than an oversight.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Use",
		meta = (ClampMin = "0.0"))
	float UseCooldown = 1.f;

	/** Played while consuming. The effects apply regardless. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Use")
	TObjectPtr<UAnimMontage> UseMontage;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGConsumable", GetFName());
	}
};
