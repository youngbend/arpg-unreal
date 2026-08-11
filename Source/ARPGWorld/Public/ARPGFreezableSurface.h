// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ARPGFreezableSurface.generated.h"

UINTERFACE(MinimalAPI)
class UARPGFreezableSurface : public UInterface
{
	GENERATED_BODY()
};

/**
 * Something with a surface an ice shard can freeze part of.
 *
 * WHY THIS EXISTS. TrySolidify used to require one side to literally be an
 * AARPGFluidPool, which quietly meant only a body the fluid system had spawned
 * could ever freeze. An authored river -- the case the whole reservoir idea was
 * built for -- failed the cast and fell through to an ordinary energy trade, so
 * an ice shard into a river made ice and no floe.
 *
 * The three questions freezing actually asks are the three below, and neither of
 * them is "are you a pool":
 *
 *   WHAT SHAPE are you, near where I hit you?
 *   HOW HIGH is the surface I am freezing?
 *   TAKE this much of yourself away.
 *
 * A puddle answers with its whole ring and shrinks. A river answers with a patch
 * of itself around the contact and IGNORES the third, because bottomless is what
 * a reservoir means everywhere else in the codebase -- Consume is already a no-op
 * for one -- and freezing was the one place that forgot.
 */
class ARPGWORLD_API IARPGFreezableSurface
{
	GENERATED_BODY()

public:
	/**
	 * The part of this surface around a contact, as a polygon in world XY.
	 *
	 * Bounded by the contact rather than returned whole, because a river is
	 * kilometres long and only the metre an ice shard touched is a candidate for
	 * freezing. A body small enough to hand back entirely may ignore both
	 * arguments and do so.
	 *
	 * Empty when there is nothing there to freeze.
	 */
	virtual TArray<FVector2D> GetFreezableFootprint(const FVector2D& Centre, double Radius) const = 0;

	/** Height of the surface that froze, at a point within that footprint. */
	virtual float GetFreezableSurfaceHeight(const FVector2D& At) const = 0;

	/**
	 * Takes this much area out of the body, because the fluid is genuinely used
	 * up by freezing.
	 *
	 * @return true when too little is left to go on being a body at all, which
	 *         is the caller's cue to retire it. A bottomless surface takes
	 *         nothing and always returns false.
	 */
	virtual bool ConsumeFreezableArea(double Area) = 0;
};
