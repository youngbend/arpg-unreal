// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGCombatTypes.generated.h"

/**
 * Which hitbox an attack arms during its active window.
 *
 * Lives in its own header because both the attack data asset (which says which
 * hitbox to use) and the hitbox component (which says which one it is) need it,
 * and neither should have to include the other.
 */
UENUM(BlueprintType)
enum class EARPGHitboxSource : uint8
{
	/** The equipped weapon's blade. Most attacks. */
	Weapon,
	/**
	 * The character's own body. Kicks and unarmed strikes, which have to land
	 * mid-combo without needing the sword to reach.
	 */
	BodyFoot
};

/**
 * The section names an attack montage is expected to use.
 *
 * These are the six clip slots Godot's AttackDefinition carried as separate
 * animation-name fields, which animation.gd sequenced by hand. Naming them here
 * rather than scattering string literals across the ability is what lets the
 * ability ask a montage questions -- how long is the windup, is there a recovery
 * to fall out to -- instead of tracking elapsed time itself.
 *
 * Only Windup and Active are required. A montage that omits Recovery simply
 * plays to its end, and a charge or channel that cannot find Windup falls back
 * to linear playback rather than failing.
 */
namespace ARPGMontageSections
{
	ARPGCOMBAT_API extern const FName Windup;
	ARPGCOMBAT_API extern const FName Active;
	ARPGCOMBAT_API extern const FName Gap;
	ARPGCOMBAT_API extern const FName Active2;
	ARPGCOMBAT_API extern const FName Landing;
	ARPGCOMBAT_API extern const FName Recovery;
}
