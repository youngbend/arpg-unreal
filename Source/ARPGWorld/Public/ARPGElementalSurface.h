// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ARPGElementalSurface.generated.h"

UINTERFACE(MinimalAPI)
class UARPGElementalSurface : public UInterface
{
	GENERATED_BODY()
};

/**
 * A body of element LYING somewhere, with an area that a reaction can take from.
 *
 * WHY THIS EXISTS. Two separate paths each needed the same three answers and
 * each hard-coded a different way of getting them. TrySolidify required one side
 * to literally be an AARPGFluidPool, so an authored river could never be frozen.
 * And a reaction that spent a body's energy changed nothing about the body at
 * all -- a fireball into a puddle made steam and left the puddle exactly as big,
 * because the pool recomputes its energy from its area on the next weather tick
 * and quietly discarded whatever the reaction had spent.
 *
 * The questions are the same in both cases, and none of them is "what class are
 * you":
 *
 *   WHAT SHAPE are you, near where I hit you?
 *   HOW HIGH is your surface?
 *   TAKE this much area away.
 *   HOW MUCH ENERGY is a unit of your area worth?
 *
 * The last one is what converts a reaction's spend into a loss of ground, so the
 * combination table's consumption rates already decide how fast a fireball boils
 * a puddle and how fast it melts a floe. No second set of numbers.
 *
 * A puddle shrinks. A floe shrinks and eventually melts back into water. A river
 * takes nothing and is never used up, because bottomless is what bReservoir
 * means everywhere else -- Consume already refuses to spend one.
 */
class ARPGWORLD_API IARPGElementalSurface
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
	 * Empty when there is nothing there.
	 */
	virtual TArray<FVector2D> GetSurfaceFootprint(const FVector2D& Centre, double Radius) const = 0;

	/** Height of the surface at a point within that footprint. */
	virtual float GetSurfaceLevelAt(const FVector2D& At) const = 0;

	/**
	 * Takes this much area out of the body.
	 *
	 * @return true when too little is left to go on being a body at all, which is
	 *         the caller's cue to retire it. A bottomless surface takes nothing
	 *         and always returns false.
	 */
	virtual bool ConsumeSurfaceArea(double Area) = 0;

	/**
	 * Energy per unit of area, so a reaction's spend converts into ground lost.
	 *
	 * Zero means a reaction never eats this: it is the same statement as
	 * ConsumeSurfaceArea refusing, made early enough to skip the arithmetic.
	 */
	virtual float GetSurfaceEnergyDensity() const = 0;
};
