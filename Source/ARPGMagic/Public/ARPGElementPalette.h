// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ARPGElementPalette.generated.h"

/**
 * The few colours an element is, so a placeholder can stand in for any VFX that
 * has not been authored yet. Port of Godot's ElementPalette.
 *
 * THREE STOPS, because that is already what the project draws -- core, the
 * readable middle, and a fading edge. Naming that convention once beats retyping
 * it into every new effect.
 *
 * This is deliberately NOT a way to generate finished VFX. A palette cannot say
 * that ice shatters and air billows; those differences are why the real effects
 * stay hand-authored. What it buys is that an element with nothing authored yet
 * is VISIBLE and recognisably itself instead of invisible -- which is the state
 * most elements are in during development, and which otherwise makes elemental
 * interactions impossible to test.
 */
UCLASS(BlueprintType)
class ARPGMAGIC_API UARPGElementPalette : public UDataAsset
{
	GENERATED_BODY()

public:
	/**
	 * Brightest centre: the heart of a flame, the core of an arc. Usually close
	 * to white, so the effect reads as hot rather than merely tinted.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Palette")
	FLinearColor Core = FLinearColor(1.f, 1.f, 1.f, 1.f);

	/** The element's actual colour -- what someone would name it. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Palette")
	FLinearColor Glow = FLinearColor(0.6f, 0.7f, 1.f, 0.9f);

	/**
	 * Outer and fading: smoke, char, the transparent tail of a particle. Its
	 * alpha is the ramp's final alpha, so leave it at 0 for effects that should
	 * fade out rather than end abruptly.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Palette")
	FLinearColor Edge = FLinearColor(0.3f, 0.4f, 1.f, 0.f);

	/** Multiplies emission and light energy. Lightning wants far more than smoke. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Palette",
		meta = (ClampMin = "0.0"))
	float EmissionStrength = 3.f;

	/** The three stops as a curve, for driving a particle colour ramp. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	void GetGradientStops(FLinearColor& OutCore, FLinearColor& OutGlow, FLinearColor& OutEdge) const
	{
		OutCore = Core;
		OutGlow = Glow;
		OutEdge = Edge;
	}
};
