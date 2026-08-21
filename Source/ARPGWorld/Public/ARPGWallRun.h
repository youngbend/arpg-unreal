// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGWallRun.generated.h"

/**
 * Fluid making its way down the outside of a slab.
 *
 * WHAT A HEIGHTFIELD CANNOT HOLD. The field says how deep the film lies on every
 * cell TOP, which is the whole of z = f(x, y) -- and a vertical face is many
 * heights at one column, the one thing that form cannot express. So the moment
 * melt reaches a rim it leaves the field entirely, and this is what carries it
 * from there to the ground.
 *
 * NOT A SIMULATION, and deliberately not. A run is a volume, a place on the rim
 * it went over, and how far down it has got. It creeps at the fluid's own
 * WallSpeed, and when its leading edge reaches the foot it hands its volume to
 * the pool below. That is enough to read as lava oozing down a wall and pooling
 * at the bottom, and it costs a float per run per tick.
 */
USTRUCT(BlueprintType)
struct ARPGWORLD_API FARPGWallRun
{
	GENERATED_BODY()

	/** Where on the rim it went over, in the field's frame. */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Fluid")
	FVector2D At = FVector2D::ZeroVector;

	/** Which way it is running down: the step to the missing neighbour. */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Fluid")
	FIntPoint Side = FIntPoint::ZeroValue;

	/** How much is in it. Conserved: this is the melt, in transit. */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Fluid")
	double Volume = 0.0;

	/** Slab-local Z of the leading edge, descending toward Foot. */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Fluid")
	float Front = 0.f;

	/** Slab-local Z where the face runs out and the run has arrived. */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Fluid")
	float Foot = 0.f;

	/** How thick the sheet is drawn, from the film that fed it. */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Fluid")
	float Thickness = 1.f;

	bool HasArrived() const { return Front <= Foot; }
};
