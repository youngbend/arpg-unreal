// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ARPGSwingAugment.generated.h"

class UAbilitySystemComponent;
class UARPGHitboxComponent;

UINTERFACE(MinimalAPI, Blueprintable)
class UARPGSwingAugment : public UInterface
{
	GENERATED_BODY()
};

/**
 * Something that adds a payload to a weapon swing it does not own.
 *
 * THIS EXISTS TO CROSS A MODULE BOUNDARY THE WRONG WAY. An imbued swing is a
 * magic feature delivered by a combat one, and ARPGMagic sits ON TOP of
 * ARPGCombat -- UARPGGameplayAbility_MeleeAttack cannot name
 * UARPGGameplayAbility_Imbue and never will. So the swing asks "is anything
 * riding this?" through an interface declared on its own side, and the imbue
 * answers from the other side of the DAG. Same shape as IARPGVfxFittable: the
 * caller offers the data, the implementer decides what to do with it.
 *
 * THE SWING OWNS THE TIMING, not the augment. Only the attack knows its motion
 * value, which of its windows is arming, and whether it was cancelled before
 * reaching one -- so the augment is told, never asked.
 */
class ARPGCOMBAT_API IARPGSwingAugment
{
	GENERATED_BODY()

public:
	/**
	 * Stamp whatever this augment contributes onto one hitbox window, scaled by
	 * that window's motion value.
	 *
	 * Called immediately before the hitbox is armed, so the first sweep already
	 * carries the payload, and once PER WINDOW -- a double-hit swing asks twice,
	 * with each window's own motion value.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "ARPG|Combat")
	void ArmSwingAugment(UARPGHitboxComponent* Hitbox, float MotionValue);

	/**
	 * The attack has ended, cancelled or not. Spend here rather than on the
	 * window, so a three-window swing spends one augment and not three.
	 *
	 * Called on EVERY attack that ends, including ones that never armed
	 * anything: an augment that was never delivered has to survive for the next
	 * swing, because the player has already paid for it.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "ARPG|Combat")
	void NotifySwingEnded();
};

namespace ARPGSwingAugments
{
	/**
	 * The augment riding this character's swings, or null when there is none --
	 * which is the ordinary case, and costs one walk of a list that holds a
	 * dozen entries.
	 *
	 * Returns the UObject rather than IARPGSwingAugment*, so call through
	 * IARPGSwingAugment::Execute_*. A Blueprint ability implementing this is not
	 * castable to the native interface pointer and would be silently skipped.
	 */
	ARPGCOMBAT_API UObject* FindActive(UAbilitySystemComponent* ASC);
}
