// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "ARPGStatusEffects.generated.h"

/**
 * The four status effects ported from project/combat/status_effects/*.tres.
 *
 * Defined in C++ rather than as content so the port is version-controlled and
 * testable headlessly. Blueprint subclasses remain the right place for tuning;
 * they inherit all the wiring below.
 *
 * WHAT THIS PORT DELETED. Two of the four carried GDScript in Godot, and
 * neither needs any here:
 *
 *   wet.gd granted fire resistance through CombatComponent's hand-rolled timed
 *     bonus list. Because _on_apply fires on EVERY re-application -- and
 *     standing in water re-applies once a second -- a naive implementation
 *     compounded the bonus, so the script carried a meta flag to seed once and
 *     an _on_stack_changed hook to increment thereafter. In GAS this is one
 *     modifier: the aggregator owns it for the effect's lifetime and
 *     re-evaluates on stack change. The bug class disappears with the code.
 *
 *   weakened.gd called add_damage_amp() with a timed multiplier. That is a
 *     Multiply modifier on DamageAmpMultiplier.
 *
 * That is the clearest argument the GAS choice has produced: behaviour that was
 * imperative, order-sensitive and re-entrancy-prone becomes declarative data.
 */

/** Shared wiring: status component, application gating, and the ARPG damage execution for ticks. */
UCLASS(Abstract)
class ARPGCOMBAT_API UARPGStatusGameplayEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UARPGStatusGameplayEffect();
};

/**
 * Burning. 5s, up to 5 stacks, 5 damage per stack per second.
 *
 * Tick damage routes through UARPGDamageExecution rather than a plain modifier,
 * so fire resistance applies to it exactly as it does to a fireball -- which is
 * what makes Wet's resistance bonus actually reduce burn damage.
 */
UCLASS()
class ARPGCOMBAT_API UARPGStatusEffect_Burning : public UARPGStatusGameplayEffect
{
	GENERATED_BODY()

public:
	UARPGStatusEffect_Burning();
};

/** Shocked. 3s, up to 3 stacks, 2 damage per stack every 0.5s. */
UCLASS()
class ARPGCOMBAT_API UARPGStatusEffect_Shocked : public UARPGStatusGameplayEffect
{
	GENERATED_BODY()

public:
	UARPGStatusEffect_Shocked();
};

/**
 * Wet. 6s, up to 3 stacks. Grants fire resistance per stack and strips Burning.
 *
 * Not a debuff -- being soaked is protective here, which is why is_debuff was
 * false on the .tres.
 */
UCLASS()
class ARPGCOMBAT_API UARPGStatusEffect_Wet : public UARPGStatusGameplayEffect
{
	GENERATED_BODY()

public:
	UARPGStatusEffect_Wet();

	/** Additive fire resistance per stack. At 3 stacks that is 30% mitigation. */
	static constexpr float FireResistancePerStack = 0.1f;
};

/**
 * Weakened. 8s, reduces all outgoing damage to 50%.
 *
 * Must stay finite: the Godot implementation noted that its damage-amp hook only
 * supported timed bonuses. That constraint is gone here, but a permanent
 * halving of a character's damage is not what the effect means.
 */
UCLASS()
class ARPGCOMBAT_API UARPGStatusEffect_Weakened : public UARPGStatusGameplayEffect
{
	GENERATED_BODY()

public:
	UARPGStatusEffect_Weakened();

	static constexpr float DamageAmpMultiplier = 0.5f;
};
