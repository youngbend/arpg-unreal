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
};
