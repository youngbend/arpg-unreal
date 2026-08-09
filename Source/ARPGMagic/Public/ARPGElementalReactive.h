// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ARPGElementalReactive.generated.h"

class UARPGMagicElement;

UINTERFACE(MinimalAPI, BlueprintType)
class UARPGElementalReactive : public UInterface
{
	GENERATED_BODY()
};

/**
 * How a spell reacts to meeting another element. Port of Godot's duck-typed
 * on_elemental_reaction hook.
 *
 * ONE NUMBER EXPRESSES ALL THREE OUTCOMES. Remaining is the fraction of the
 * volume's ORIGINAL energy still left:
 *
 *   0        fully spent -- finish.
 *   0 to 1   partly consumed -- scale damage and visuals down and carry on.
 *   above 1  AMPLIFIED -- it ate the other element and should grow.
 *
 * An effect that simply multiplies by Remaining handles all three without
 * branching, which is why the fraction is reported rather than a state enum.
 *
 * IMPLEMENTING THIS IS OPTIONAL. An effect that does not gets sensible defaults
 * applied for it -- see UARPGElementalVolumeComponent::ApplyDefaultReaction --
 * so a new projectile needs no reaction code at all and only overrides when it
 * wants something the default cannot express.
 */
class ARPGMAGIC_API IARPGElementalReactive
{
	GENERATED_BODY()

public:
	/**
	 * @param Consumed  Energy lost. NEGATIVE when the volume gained instead,
	 *                  which together with Remaining > 1 reads as "you ate it".
	 * @param Remaining Fraction of original energy left -- see the class comment.
	 * @param Product   What the reaction produced, or null when the two merely
	 *                  neutralised each other.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "ARPG|Magic")
	void OnElementalReaction(float Consumed, float Remaining, UARPGMagicElement* Product);
};
