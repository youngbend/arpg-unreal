// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ARPGMagicSettings.generated.h"

class AActor;

/**
 * The shared stand-ins an element falls back to when a VFX slot is unauthored.
 *
 * ONE PLACE ON PURPOSE. Editing these changes every placeholder at once, which
 * is the point: they are meant to read as "not final yet", uniformly. In Godot
 * these were hard-coded res:// paths inside MagicElement; here they are project
 * settings so a designer can point them somewhere without a rebuild.
 *
 * Replacing a placeholder is exactly "fill in the real slot on the element" --
 * the fallback only triggers on an empty slot, so there is nothing to un-wire
 * and no flag to remember.
 *
 * All four DEFAULT to the built-in stand-ins, so magic is visible in a fresh
 * checkout with no configuration at all. That matters more than it sounds: a
 * fallback system whose fallbacks have to be wired up first does not help the
 * person it exists for, and every element in development is in exactly the
 * state these cover.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "ARPG Magic"))
class ARPGMAGIC_API UARPGMagicSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UARPGMagicSettings();

	virtual FName GetCategoryName() const override { return TEXT("Game"); }

	/** Held in hand while an element is readied. */
	UPROPERTY(EditAnywhere, Config, Category = "Placeholders")
	TSoftClassPtr<AActor> PlaceholderHand;

	/** Stationary puff, for burst, emanate, cloak and collision. */
	UPROPERTY(EditAnywhere, Config, Category = "Placeholders")
	TSoftClassPtr<AActor> PlaceholderDischarge;

	/**
	 * Separate from PlaceholderDischarge because a projectile is not a
	 * stationary puff: it has to travel, sweep for collisions and die on impact.
	 * Standing in for a Project discharge with something that just sits at the
	 * caster's feet would misrepresent the spell badly enough to be useless for
	 * testing.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Placeholders")
	TSoftClassPtr<AActor> PlaceholderProjectile;

	/** Attached to the weapon during an imbued attack. */
	UPROPERTY(EditAnywhere, Config, Category = "Placeholders")
	TSoftClassPtr<AActor> PlaceholderImbue;
};
