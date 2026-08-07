// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "AttributeSet.h"
#include "GameplayEffectTypes.h"
#include "ARPGDamageTypeAsset.generated.h"

/**
 * A category of damage -- Fire, Physical, Lightning. The direct port of Godot's
 * DamageType resource, one asset per .tres.
 *
 * THE RESISTANCE INDIRECTION. Godot looked resistance up by string id in a
 * Dictionary. GAS needs a real FGameplayAttribute, so the mapping lives here as
 * data: the asset names which attribute on UARPGResistanceSet answers for this
 * damage type. UARPGDamageExecution captures every resistance attribute up front
 * and picks between them using this field, so authoring a damage type is still
 * "make an asset", not "edit the execution".
 */
UCLASS(BlueprintType)
class ARPGCORE_API UARPGDamageTypeAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Identity, e.g. Damage.Fire. Stamped onto the effect context by the hitbox. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity",
		meta = (Categories = "Damage"))
	FGameplayTag DamageTypeTag;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FText DisplayName;

	/** Floating combat text / VFX tint. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FLinearColor Color = FLinearColor::White;

	/**
	 * Replaces the CATEGORY_* bitfield. Two entries change the maths:
	 *   Damage.Category.Physical  flat armor applies before resistance
	 *   Damage.Category.True      all resistance is skipped
	 * Damage.Category.Healing is handled earlier still -- it routes to
	 * IncomingHealing and skips mitigation entirely.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity",
		meta = (Categories = "Damage.Category"))
	FGameplayTagContainer Categories;

	/**
	 * Which attribute on UARPGResistanceSet mitigates this type. Leave unset for
	 * a type nothing resists (fall damage) -- the execution then treats
	 * resistance as 0 rather than guessing.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Mitigation")
	FGameplayAttribute ResistanceAttribute;

	/** Fraction of the target's resistance ignored, 0-1. Overridable per hit. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Mitigation",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DefaultPenetration = 0.f;

	/**
	 * Floor for effective resistance. -1 means a fully vulnerable target takes
	 * double damage; raise toward 0 for a type that should never over-hit.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Mitigation",
		meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float MinResistance = -1.f;

	UFUNCTION(BlueprintPure, Category = "ARPG|Damage")
	bool HasCategory(FGameplayTag Category) const
	{
		return Categories.HasTag(Category);
	}

	/**
	 * Per-type proc hook -- Fire rolling to apply Burning, Lightning rolling to
	 * stun. The direct equivalent of GDVIRTUAL _on_damage_applied, and the reason
	 * a designer can add a damage type's behaviour without touching C++.
	 *
	 * Called on the SERVER only, after the final number has been applied.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "ARPG|Damage")
	void OnDamageApplied(AActor* Target, float FinalDamage, const FGameplayEffectContextHandle& Context) const;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGDamageType", GetFName());
	}
};
