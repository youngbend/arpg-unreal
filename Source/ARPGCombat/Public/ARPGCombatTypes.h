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
