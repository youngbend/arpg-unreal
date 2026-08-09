// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGDischargeContext.h"
#include "ARPGMagicProgression.h"
#include "ARPGProgressionTrackerComponent.h"
#include "ARPGMagicProgressionTracker.generated.h"

/**
 * Magic proficiency, and the real implementation of IARPGMagicProgression --
 * closing the interface phase 5 deliberately left open. Port of Godot's
 * MagicProgressionTracker.
 *
 * XP COMES FROM CASTING, NOT FROM CONNECTING. Every discharge counts whether or
 * not the resulting effect lands on anything. That is the opposite of the weapon
 * tracker, and on purpose: skill with an element is about USING it, and a
 * fireball that misses still taught you something about throwing fireballs. It
 * also means an element that heals or debuffs -- and so never "hits" anyone in
 * the damage sense -- progresses at all, which the weapon rule would forbid.
 *
 * The gate this feeds is the one that decides whether the player may hold two
 * elements at once, so a tracker present and empty is meaningfully different
 * from no tracker at all: an untrained caster is GATED, while a caster with no
 * progression system is exempt. See UARPGMagicComponent.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGMAGIC_API UARPGMagicProgressionTracker : public UARPGProgressionTrackerComponent,
	public IARPGMagicProgression
{
	GENERATED_BODY()

public:
	/** Multiplies a discharge's computed damage into progression XP. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Progression",
		meta = (ClampMin = "0.0"))
	float XPPerDamage = 1.f;

	// IARPGMagicProgression
	virtual float GetEffectiveElementLevel_Implementation(FGameplayTag ElementTag) const override;
	virtual float GetElementDamageMultiplier_Implementation(FGameplayTag ElementTag) const override;

protected:
	virtual void BindXPSource() override;

	UFUNCTION()
	void HandleDischargeExecuted(const FARPGDischargeContext& Context);
};
