// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectExecutionCalculation.h"
#include "ARPGDamageExecution.generated.h"

/**
 * The damage pipeline. Port of CombatComponent::receive_damage().
 *
 * ORDER OF OPERATIONS, preserved exactly from the Godot original because the
 * order is what the tuning was built against:
 *
 *   1. Healing bypass      Damage.Category.Healing routes to IncomingHealing and
 *                          skips armor, resistance and interception entirely.
 *   2. Raw                 (SetByCaller Data.Damage + AttackPower) * DamageAmpMultiplier
 *   3. Crit                * CritMultiplier when the context says so. The ROLL
 *                          happens in the hitbox (server-side), not here.
 *   4. Armor               PHYSICAL only, flat, subtracted BEFORE resistance:
 *                            raw = max(0, raw - BaseArmor)
 *   5. Resistance          effective = clamp(resist * (1 - penetration),
 *                                            MinResistance, 1.0)
 *                          Damage.Category.True skips this entirely.
 *   6. Final               raw * (1 - effective)
 *
 * Armor being flat-and-before-resistance is deliberate and easy to get backwards:
 * it makes armor strong against many small physical hits and weak against one
 * large one, which is the opposite of what resistance does.
 *
 * NOT YET IMPLEMENTED, and where they slot in:
 *   Parry/block interception (phase 3) multiplies the post-resistance number and
 *     redirects poise to the ATTACKER instead of the victim.
 *   Cloak absorption (phase 5) subtracts from the fully-mitigated number last.
 * Both are marked at their exact insertion points below.
 *
 * THE RESISTANCE LOOKUP. Every resistance attribute is captured up front (GAS
 * requires capture definitions to be static), then the damage type asset names
 * which one to actually read. That keeps damage types authored as data while
 * satisfying GAS's static-capture requirement.
 */
UCLASS()
class ARPGCOMBAT_API UARPGDamageExecution : public UGameplayEffectExecutionCalculation
{
	GENERATED_BODY()

public:
	UARPGDamageExecution();

	virtual void Execute_Implementation(
		const FGameplayEffectCustomExecutionParameters& ExecutionParams,
		FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const override;
};
