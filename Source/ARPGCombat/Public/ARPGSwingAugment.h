// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGCombatTypes.h"
#include "UObject/Interface.h"
#include "ARPGSwingAugment.generated.h"

class UAbilitySystemComponent;
class UARPGAttackDefinition;
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
	 * The attack this augment performs INSTEAD of the one the combo tree would
	 * have chosen for this button, or null to leave the tree's choice alone.
	 *
	 * Asked before the tree is consulted at all, and answered per button, so one
	 * augment can pre-empt heavy while leaving light to the weapon. Returning
	 * null for a button is not a failure -- it is how an augment that only
	 * defines one move stays usable on the other two.
	 *
	 * WHAT THE COMBO COMPONENT DOES WITH IT: runs it as a leaf, which is the
	 * existing vocabulary for "this beat terminates the chain". Nothing follows
	 * it, so the next press starts a fresh chain from root, and the attack's own
	 * FinisherLockout applies as it would on any other leaf.
	 *
	 * The augment is NOT told whether the attack it named actually ran; stamina,
	 * a flinch lock or a missing montage can still refuse it. Anything that must
	 * know goes through ArmSwingAugment and NotifySwingEnded like everything else.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "ARPG|Combat")
	UARPGAttackDefinition* GetSwingAttackOverride(EARPGAttackInput Input);

	/**
	 * Bring up whatever this augment contributes to one hitbox window, scaled by
	 * that window's motion value.
	 *
	 * Called immediately before the swing's own hitbox is armed, and once PER
	 * WINDOW -- a double-hit swing asks twice, with each window's own motion
	 * value.
	 *
	 * SwingHitbox is the hitbox the attack is about to arm. It is passed as an
	 * ANCHOR, not a target: the augment reads its transform, its trace settings
	 * and its reach to build something alongside it, and must not rewrite its
	 * payload -- that belongs to the weapon.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "ARPG|Combat")
	void ArmSwingAugment(UARPGHitboxComponent* SwingHitbox, float MotionValue);

	/**
	 * The window closed. Take down anything ArmSwingAugment brought up; the
	 * augment itself lives on until NotifySwingEnded.
	 *
	 * Called from the melee ability's disarm path, which runs on every exit
	 * including cancellation -- so an interrupted swing cannot leave a live
	 * elemental hitbox sweeping behind it.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "ARPG|Combat")
	void DisarmSwingAugment();

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
