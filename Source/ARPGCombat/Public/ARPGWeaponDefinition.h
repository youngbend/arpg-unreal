// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "ARPGWeaponDefinition.generated.h"

class UARPGDamageTypeAsset;
class UARPGWeaponAttackTree;
class USkeletalMesh;
class UStaticMesh;

UENUM(BlueprintType)
enum class EARPGWeaponType : uint8
{
	Unarmed,
	Sword,
	Axe,
	Mace,
	Spear,
	Dagger,
	Greatsword,
	// Catalysts use RT+face discharges instead of melee attacks. The split is
	// derived from the type rather than authored as a flag, so a new catalyst
	// cannot be added and forget to declare itself one.
	Staff,
	Wand,
	Orb
};

UENUM(BlueprintType)
enum class EARPGRarity : uint8
{
	Common,
	Uncommon,
	Rare,
	Epic,
	Legendary
};

/** Where the weapon rides while sheathed. Resolved to a socket on the rig. */
UENUM(BlueprintType)
enum class EARPGSheathPoint : uint8
{
	Hip,
	Back
};

/**
 * One rolled affix on a weapon. Port of Godot's WeaponModifier.
 *
 * A struct rather than an instanced object: modifiers are pure numbers with no
 * behaviour, and keeping them by value means a weapon's whole stat block
 * serialises inline instead of as a fan of subobjects.
 */
USTRUCT(BlueprintType)
struct FARPGWeaponModifier
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Modifier")
	FText DisplayName;

	/** Added to base damage before the motion value scales it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Modifier")
	float FlatDamageBonus = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Modifier",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CritChanceBonus = 0.f;

	/**
	 * Per-damage-type multipliers, keyed by damage type tag. Additive with each
	 * other, then applied multiplicatively to the swing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Modifier",
		meta = (Categories = "Damage"))
	TMap<FGameplayTag, float> TypeMultipliers;
};

/**
 * Static data for a weapon type. Port of Godot's WeaponDefinition.
 *
 * THE ATTACK TREE LIVES HERE, not on the character. A weapon IS its moveset --
 * equipping a greatsword should change what every button does, and a character
 * that owns its own tree cannot express that. This is also what removes the
 * hard-coded tree path the character previously carried.
 */
UCLASS(BlueprintType)
class ARPGCOMBAT_API UARPGWeaponDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	// --- Identity -------------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FName WeaponId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FText DisplayName;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	EARPGWeaponType WeaponType = EARPGWeaponType::Unarmed;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	EARPGRarity Rarity = EARPGRarity::Common;

	// --- Combat ---------------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat",
		meta = (ClampMin = "0.0"))
	float BaseDamage = 10.f;

	/**
	 * Reach, in centimetres. Doubles as the AI's attack range so
	 * BTCheckAttackRange stays in step with the hitbox without a second property
	 * that can drift out of sync.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat",
		meta = (ClampMin = "0.0"))
	float Reach = 150.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat")
	TObjectPtr<UARPGDamageTypeAsset> BaseDamageType;

	/** Every combo path this weapon can perform. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat")
	TObjectPtr<UARPGWeaponAttackTree> AttackTree;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat")
	TArray<FARPGWeaponModifier> Modifiers;

	// --- Presentation ---------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	TSoftObjectPtr<UStaticMesh> WeaponMesh;

	/** Socket on the character rig the weapon is held in while drawn. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	FName HandSocket = TEXT("hand_r");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	EARPGSheathPoint SheathPoint = EARPGSheathPoint::Hip;

	// --- Derived --------------------------------------------------------------

	/** Catalysts cast rather than swing. Derived from the type, never authored. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Weapon")
	bool IsCatalyst() const
	{
		return WeaponType == EARPGWeaponType::Staff
			|| WeaponType == EARPGWeaponType::Wand
			|| WeaponType == EARPGWeaponType::Orb;
	}

	UFUNCTION(BlueprintPure, Category = "ARPG|Weapon")
	float GetTotalFlatDamageBonus() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Weapon")
	float GetTotalCritChanceBonus() const;

	/** Summed multiplier for a damage type. 1.0 when no modifier mentions it. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Weapon")
	float GetTotalTypeMultiplier(FGameplayTag DamageTypeTag) const;

	/**
	 * Non-imbued swing damage:
	 *   (BaseDamage + flat bonuses) * MotionValue * type multiplier
	 *
	 * Prefer UARPGWeaponComponent::GetEffectiveDamage, which wraps this with the
	 * wielder's mastery and outgoing damage-amp -- the same buffs that scale
	 * spells, so a Weakened status weakens swings exactly as it weakens magic.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Weapon")
	float GetEffectiveDamage(float MotionValue) const;

	/** Stable lowercase id, used as the mastery progression key. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Weapon")
	static FName WeaponTypeToName(EARPGWeaponType Type);

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGWeapon", GetFName());
	}
};
