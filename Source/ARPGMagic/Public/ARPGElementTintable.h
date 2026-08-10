// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ARPGElementTintable.generated.h"

class AActor;
class UARPGElementPalette;
class UARPGMagicElement;

UINTERFACE(MinimalAPI, Blueprintable)
class UARPGElementTintable : public UInterface
{
	GENERATED_BODY()
};

/**
 * A visual that takes its colours from whichever element cast it.
 *
 * Optional, and the same shape as IARPGVfxFittable: the spawn point offers the
 * data, the effect decides whether it cares. An authored fire effect that is
 * already orange simply does not implement this.
 *
 * What it buys is that ONE effect asset can serve every element -- which is the
 * entire reason the placeholders work at all, and is worth having available to
 * real effects too. A generic arc, a generic splash and a generic bloom cover a
 * lot of ground once they can be told what colour to be.
 */
class ARPGMAGIC_API IARPGElementTintable
{
	GENERATED_BODY()

public:
	/**
	 * Called once, immediately after spawning, before the effect begins play.
	 *
	 * Palette may be null -- an element whose author has opted out of the whole
	 * placeholder system. Treat that as "use my authored colours".
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "ARPG|Magic")
	void ApplyPalette(const UARPGElementPalette* Palette);
};

namespace ARPGElementTint
{
	/**
	 * Hands an element's palette to a freshly spawned actor, if it wants one.
	 *
	 * Called at the SPAWN POINT rather than from each effect's own BeginPlay,
	 * for the reason AARPGDischargeEffect stamps its own hitbox there: an effect
	 * asset should be art and nothing else. Every place that spawns an elemental
	 * visual routes through here, so a new spawn site cannot forget to tint and
	 * a new effect cannot forget to ask.
	 *
	 * Safe with any argument null.
	 */
	ARPGMAGIC_API void Apply(AActor* Instance, const UARPGMagicElement* Element);
}
