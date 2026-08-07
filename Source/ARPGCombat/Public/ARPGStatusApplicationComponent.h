// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectComponent.h"
#include "ARPGStatusApplicationComponent.generated.h"

/**
 * Gates status application on the two things GAS does not check for us:
 * whether the target is alive, and whether it resists this status.
 *
 * THE ALIVE GATE IS NOT COSMETIC. It fixes a specific bug the Godot project
 * documented at length in CombatComponent::apply_status_effect. Most statuses
 * arrive through the damage pipeline, which already refuses to run on a corpse
 * -- but several callers bypass it entirely: environmental spread pushing
 * exposure onto a body lying in a fire, and a cloak re-applying its self-effect
 * on a timer with nothing cancelling it on death. Either could land a fresh
 * stack on a corpse, and nothing would ever clear it: status ticking is gated on
 * being alive, so the stack neither counts down nor expires. Checking here, at
 * the single point every application funnels through, closes it for every caller
 * at once rather than chasing each one.
 *
 * Effect-side application chance is NOT here -- that is
 * UChanceToApplyGameplayEffectComponent, which ships with the engine. This is
 * only the target's own say in the matter.
 */
UCLASS(DisplayName = "ARPG Status Application")
class ARPGCOMBAT_API UARPGStatusApplicationComponent : public UGameplayEffectComponent
{
	GENERATED_BODY()

public:
	UARPGStatusApplicationComponent();

	/**
	 * Refuse application to a target at or below zero health. On by default:
	 * an effect that genuinely should affect corpses is the rare case and
	 * should say so explicitly.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Application")
	bool bRequireTargetAlive = true;

	/**
	 * Consult the target's UARPGStatusResistanceComponent for immunity and a
	 * resistance roll, keyed on this effect's StatusTag.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Application")
	bool bRespectTargetResistance = true;

	virtual bool CanGameplayEffectApply(
		const FActiveGameplayEffectsContainer& ActiveGEContainer,
		const FGameplayEffectSpec& GESpec) const override;
};
