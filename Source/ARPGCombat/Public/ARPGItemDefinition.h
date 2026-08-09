// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ARPGItemDefinition.generated.h"

class UARPGArmorDefinition;
class UARPGConsumableDefinition;
class UARPGWeaponDefinition;
class UTexture2D;

/**
 * What an item IS, which decides which payload matters.
 *
 * Values are appended to, never reordered: they are serialised into every item
 * asset that exists.
 */
UENUM(BlueprintType)
enum class EARPGItemType : uint8
{
	Weapon = 0,
	Consumable = 1,
	Material = 2,
	Key = 3,
	Armor = 4
};

/**
 * Static data for one item type. Port of Godot's ItemDefinition.
 *
 * ONE CLASS WITH THREE OPTIONAL PAYLOADS rather than a subclass per type. An
 * inventory has to hold all of them in one list, sort them together, and stack
 * them by the same rules; a class hierarchy would push a cast into every one of
 * those places to answer questions the base class can answer directly.
 */
UCLASS(BlueprintType)
class ARPGCOMBAT_API UARPGItemDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Stable id. Quick slots hold this rather than the asset -- see the quick slot component. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FName ItemId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FText DisplayName;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FText Description;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	TSoftObjectPtr<UTexture2D> Icon;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	EARPGItemType ItemType = EARPGItemType::Material;

	/** Units per slot. Weapons, armour and key items should stay at 1. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity",
		meta = (ClampMin = "1"))
	int32 MaxStackSize = 1;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Payload",
		meta = (EditCondition = "ItemType == EARPGItemType::Weapon"))
	TObjectPtr<UARPGWeaponDefinition> WeaponDefinition;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Payload",
		meta = (EditCondition = "ItemType == EARPGItemType::Armor"))
	TObjectPtr<UARPGArmorDefinition> ArmorDefinition;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Payload",
		meta = (EditCondition = "ItemType == EARPGItemType::Consumable"))
	TObjectPtr<UARPGConsumableDefinition> ConsumableDefinition;

	/**
	 * True for types that go in an equipment slot rather than the bag's general
	 * list. Both the equipment and inventory views split on exactly this, so they
	 * stay in agreement as new types are added.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Item")
	bool IsEquipment() const
	{
		return ItemType == EARPGItemType::Weapon || ItemType == EARPGItemType::Armor;
	}

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGItem", GetFName());
	}

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
};
