// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GameplayTagContainer.h"
#include "ARPGCombatLibrary.generated.h"

UCLASS()
class ARPGCOMBAT_API UARPGCombatLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Reads an actor's faction from the tags owned by its ability system
	 * component. Returns false when the actor has no ASC or no Faction.* tag --
	 * an unowned environmental hazard, say.
	 *
	 * Callers must treat "no faction" as "do not filter" rather than "hostile to
	 * nobody": the Godot original skipped filtering entirely in that case so an
	 * unowned hazard still damages things, and silently no-opping the hit instead
	 * is the failure mode that produces a lava floor nothing walks out of.
	 */
	static bool TryResolveFaction(const AActor* Actor, FGameplayTag& OutFaction);

	/**
	 * Port of CharacterBase::are_factions_hostile().
	 *
	 *   Neutral is hostile to nobody, in either position.
	 *   Player and Ally are one side; Enemy is the other.
	 *   Hostile exactly when the two sides differ.
	 *
	 * This is also what stops a self-fired projectile killing its own caster: an
	 * attack sourced from a Player-faction actor is never hostile to that same
	 * actor's Player-faction hurtbox.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Combat")
	static bool AreFactionsHostile(FGameplayTag FactionA, FGameplayTag FactionB);

	/** True when the source may damage the target under the faction rules above. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Combat")
	static bool CanDamage(const AActor* SourceActor, const AActor* TargetActor);

	/**
	 * Displaces a target along a hit's knockback, on the server.
	 *
	 * NEW, AND DELIBERATELY SMALL. KnockbackForce and KnockbackDirection have
	 * been travelling on the effect context since phase 1 and nothing has ever
	 * read them -- every hit in the game has been computing a shove that never
	 * happened. This is the receiver, no more: no curve, no resistance-to-
	 * knockback attribute, no prediction. It exists because a magnetic pull is
	 * not expressible without it.
	 *
	 * A NEGATIVE FORCE PULLS. Direction points from the hitbox toward the
	 * contact, so negating it drags the target back along the same line -- which
	 * is exactly a magnetic yank, and is why the attack definition's
	 * KnockbackPower had its ClampMin lifted.
	 *
	 * Characters only. Launching a static mesh is a physics problem with its own
	 * answer, and guessing at one here would be worse than doing nothing.
	 *
	 * @param bOverrideVertical  true lets the hit set vertical velocity as well,
	 *                           for a blow meant to lift. False preserves the
	 *                           target's own fall, which is right for a shove.
	 */
	static void ApplyKnockback(AActor* Target, const FVector& Direction, float Force,
		bool bOverrideVertical = false);
};
