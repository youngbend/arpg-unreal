// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGAttributeSetBase.h"
#include "ARPGOffenseSet.generated.h"

/**
 * Attacker-side scalars.
 *
 * DamageAmpMultiplier replaces CombatComponent::add_damage_amp(). In Godot that
 * was a hand-rolled vector of {multiplier, remaining} entries ticked every frame
 * and multiplied together on read. Here it is one attribute at base 1.0, and a
 * timed buff is a duration GameplayEffect with a Multiply modifier -- GAS's
 * aggregator already multiplies concurrent Multiply mods together, so
 * multiplicative stacking, expiry and replication all come free. A debuff is the
 * same thing below 1.0, exactly as the Godot version intended.
 */
UCLASS()
class ARPGCORE_API UARPGOffenseSet : public UARPGAttributeSetBase
{
	GENERATED_BODY()

public:
	UARPGOffenseSet();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Flat attacker power folded into outgoing damage by the execution. */
	UPROPERTY(BlueprintReadOnly, Category = "Offense", ReplicatedUsing = OnRep_AttackPower)
	FGameplayAttributeData AttackPower;
	ATTRIBUTE_ACCESSORS(UARPGOffenseSet, AttackPower)

	/** 0-1 probability. Rolled SERVER-SIDE ONLY -- see FARPGGameplayEffectContext::bIsCritical. */
	UPROPERTY(BlueprintReadOnly, Category = "Offense", ReplicatedUsing = OnRep_CritChance)
	FGameplayAttributeData CritChance;
	ATTRIBUTE_ACCESSORS(UARPGOffenseSet, CritChance)

	UPROPERTY(BlueprintReadOnly, Category = "Offense", ReplicatedUsing = OnRep_CritMultiplier)
	FGameplayAttributeData CritMultiplier;
	ATTRIBUTE_ACCESSORS(UARPGOffenseSet, CritMultiplier)

	/** Base 1.0. Timed buffs/debuffs stack multiplicatively via Multiply modifiers. */
	UPROPERTY(BlueprintReadOnly, Category = "Offense", ReplicatedUsing = OnRep_DamageAmpMultiplier)
	FGameplayAttributeData DamageAmpMultiplier;
	ATTRIBUTE_ACCESSORS(UARPGOffenseSet, DamageAmpMultiplier)

protected:
	UFUNCTION() void OnRep_AttackPower(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_CritChance(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_CritMultiplier(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_DamageAmpMultiplier(const FGameplayAttributeData& OldValue);
};
