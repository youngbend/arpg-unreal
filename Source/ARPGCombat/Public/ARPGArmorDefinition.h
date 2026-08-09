// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "ARPGWeaponDefinition.h" // EARPGRarity
#include "ARPGArmorDefinition.generated.h"

class USkeletalMesh;
class UARPGDamageTypeAsset;

UENUM(BlueprintType)
enum class EARPGArmorType : uint8
{
	Unarmored,
	Light,
	Medium,
	Heavy
};

/** One rolled affix on an armour piece. Mirrors FARPGWeaponModifier. */
USTRUCT(BlueprintType)
struct FARPGArmorModifier
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Modifier")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Modifier")
	float FlatArmorBonus = 0.f;

	/**
	 * Additive resistance per damage type, in the same units as
	 * UARPGResistanceSet: 0.1 is ten percent mitigation, negative is
	 * vulnerability.
	 *
	 * Keyed by the damage type ASSET, not its tag, because the asset already
	 * names which resistance attribute answers for it. Keying by tag would need
	 * a second tag-to-attribute lookup that could drift out of step with the
	 * assets.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Modifier")
	TMap<TObjectPtr<UARPGDamageTypeAsset>, float> ResistanceBonuses;
};

/**
 * Static data for an armour piece. Port of Godot's ArmorDefinition.
 *
 * Armour contributes two different things, and they behave differently in the
 * damage pipeline: FLAT armour is subtracted from physical damage before
 * resistance scales it, while RESISTANCE is a proportion applied after. That is
 * why a heavy set is strong against a flurry of small hits and comparatively
 * weak against one big one.
 */
UCLASS(BlueprintType)
class ARPGCOMBAT_API UARPGArmorDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FName ArmorId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FText DisplayName;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	EARPGArmorType ArmorType = EARPGArmorType::Unarmored;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	EARPGRarity Rarity = EARPGRarity::Common;

	/** Flat mitigation, added on top of the wearer's own BaseArmor attribute. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Defence",
		meta = (ClampMin = "0.0"))
	float ArmorValue = 0.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Defence")
	TArray<FARPGArmorModifier> Modifiers;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	TSoftObjectPtr<USkeletalMesh> ArmorMesh;

	UFUNCTION(BlueprintPure, Category = "ARPG|Armor")
	float GetTotalArmorValue() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Armor")
	float GetTotalResistanceBonus(const UARPGDamageTypeAsset* DamageType) const;

	/** Every resistance this piece grants, summed per damage type. */
	TMap<TObjectPtr<UARPGDamageTypeAsset>, float> GetAggregatedResistances() const;

	/** Stable lowercase id, for display and legacy keying. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Armor")
	static FName ArmorTypeToName(EARPGArmorType Type);

	/** The progression id for this armour's type. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Armor")
	static FGameplayTag ArmorTypeToTag(EARPGArmorType Type);

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGArmor", GetFName());
	}
};
