// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGAttributeSetBase.h"
#include "ARPGResistanceSet.generated.h"

/**
 * Per-damage-type resistances, plus flat armor.
 *
 * SEMANTICS (unchanged from Godot's CombatStats::resistances):
 *    0.0  no resistance, full damage
 *    0.5  50% mitigation
 *    1.0  immune
 *   -0.5  50% VULNERABILITY, i.e. 1.5x damage
 * The negative range is deliberate and load-bearing -- DamageType::min_resistance
 * defaults to -1.0, meaning a fully vulnerable target takes double damage.
 *
 * WHY ONE ATTRIBUTE PER TYPE. Godot keyed these by string in a Dictionary, which
 * was fully data-driven. GAS attributes are fixed C++ fields, so this is the one
 * place the port is structurally less dynamic. Adding a damage type costs three
 * lines: an attribute here, a capture in UARPGDamageExecution, and a
 * UARPGDamageTypeAsset pointing at the attribute. In exchange, a timed
 * resistance buff -- CombatComponent::add_resistance_bonus(), which was a
 * hand-ticked vector of {type_id, bonus, remaining} -- becomes an ordinary
 * duration GameplayEffect with an Additive modifier, and gains stacking, expiry,
 * replication and UI queryability for nothing. That trade is worth taking.
 *
 * Nothing looks these up by name: the mapping from damage-type tag to attribute
 * lives in the UARPGDamageTypeAsset, so the type stays authored as data even
 * though the storage is static.
 */
UCLASS()
class ARPGCORE_API UARPGResistanceSet : public UARPGAttributeSetBase
{
	GENERATED_BODY()

public:
	UARPGResistanceSet();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * Flat reduction subtracted from PHYSICAL damage before resistance scaling.
	 * Physical only -- matching CombatComponent::receive_damage, where armor is
	 * gated on DamageType::CATEGORY_PHYSICAL. Phase 4's ArmorComponent adds its
	 * bonus on top of this via an Additive modifier.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Resistance", ReplicatedUsing = OnRep_BaseArmor)
	FGameplayAttributeData BaseArmor;
	ATTRIBUTE_ACCESSORS(UARPGResistanceSet, BaseArmor)

	UPROPERTY(BlueprintReadOnly, Category = "Resistance", ReplicatedUsing = OnRep_PhysicalResistance)
	FGameplayAttributeData PhysicalResistance;
	ATTRIBUTE_ACCESSORS(UARPGResistanceSet, PhysicalResistance)

	UPROPERTY(BlueprintReadOnly, Category = "Resistance", ReplicatedUsing = OnRep_FireResistance)
	FGameplayAttributeData FireResistance;
	ATTRIBUTE_ACCESSORS(UARPGResistanceSet, FireResistance)

	UPROPERTY(BlueprintReadOnly, Category = "Resistance", ReplicatedUsing = OnRep_IceResistance)
	FGameplayAttributeData IceResistance;
	ATTRIBUTE_ACCESSORS(UARPGResistanceSet, IceResistance)

	UPROPERTY(BlueprintReadOnly, Category = "Resistance", ReplicatedUsing = OnRep_LightResistance)
	FGameplayAttributeData LightResistance;
	ATTRIBUTE_ACCESSORS(UARPGResistanceSet, LightResistance)

	UPROPERTY(BlueprintReadOnly, Category = "Resistance", ReplicatedUsing = OnRep_LightningResistance)
	FGameplayAttributeData LightningResistance;
	ATTRIBUTE_ACCESSORS(UARPGResistanceSet, LightningResistance)

	UPROPERTY(BlueprintReadOnly, Category = "Resistance", ReplicatedUsing = OnRep_WaterResistance)
	FGameplayAttributeData WaterResistance;
	ATTRIBUTE_ACCESSORS(UARPGResistanceSet, WaterResistance)

	UPROPERTY(BlueprintReadOnly, Category = "Resistance", ReplicatedUsing = OnRep_FallResistance)
	FGameplayAttributeData FallResistance;
	ATTRIBUTE_ACCESSORS(UARPGResistanceSet, FallResistance)

protected:
	UFUNCTION() void OnRep_BaseArmor(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_PhysicalResistance(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_FireResistance(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_IceResistance(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_LightResistance(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_LightningResistance(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_WaterResistance(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_FallResistance(const FGameplayAttributeData& OldValue);
};
