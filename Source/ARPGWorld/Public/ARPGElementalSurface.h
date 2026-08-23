// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGFluidGeometry.h"
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
	 * Takes a NAMED REGION out of the body, rather than an amount from wherever.
	 *
	 * WHERE IT WENT IS USUALLY THE WHOLE POINT. Freezing turns a particular patch
	 * of water into ice, and a spell breaks the part of a wall it actually hit --
	 * so taking the area off uniformly leaves the ice sitting on water that never
	 * receded, and a wall that shrinks evenly no matter where it was struck.
	 *
	 * Defaults to the amount, which is right for anything that has no shape to cut
	 * -- a river is bottomless and refuses either way.
	 *
	 * @param Region a world-space polygon. @return as ConsumeSurfaceArea.
	 */
	virtual bool ConsumeSurfaceRegion(const TArray<FVector2D>& Region)
	{
		return ConsumeSurfaceArea(ARPGFluidGeometry::PolygonArea(Region));
	}

	/**
	 * Takes a volume of fluid back INTO the body, and says whether it did.
	 *
	 * FALSE IS THE ORDINARY ANSWER AND NOT A FAILURE. A puddle would rather the
	 * water were deposited where it appeared, so the outline grows at the point
	 * the ice actually melted rather than uniformly somewhere else -- and the
	 * deposit path already merges, so saying no here gets a better result than
	 * saying yes. What answers true is a body with no outline to grow: a
	 * reservoir is bottomless in both directions, and water returned to a lake
	 * joins the lake rather than making a puddle on top of it.
	 *
	 * @param Volume in cubic cm of THIS body's fluid, already converted from
	 *        whatever was carrying it.
	 */
	virtual bool AbsorbSurfaceVolume(double Volume) = 0;

	/**
	 * Energy per unit of area, so a reaction's spend converts into ground lost.
	 *
	 * Zero means a reaction never eats this: it is the same statement as
	 * ConsumeSurfaceArea refusing, made early enough to skip the arithmetic.
	 */
	virtual float GetSurfaceEnergyDensity() const = 0;

	/**
	 * Is this XY still within the body?
	 *
	 * What a floe asks to find out whether it has anywhere to drift to, and
	 * whether it is wedged against the shore. Deliberately the same question
	 * freezing asks when it marches a footprint out to the bank, so a body only
	 * has to be able to answer "am I here" once.
	 */
	virtual bool IsSurfaceAt(const FVector2D& At) const = 0;

	/**
	 * How fast this surface is MOVING at a point, in cm/s. Zero for still water.
	 *
	 * Only a river has an answer worth giving, and it comes from the water body's
	 * own flow -- the same data the plugin's buoyancy pushes boats with. A puddle
	 * and a plain box return zero, which is not a stub: a puddle genuinely has no
	 * current, and a floe on one should sit still.
	 */
	virtual FVector2D GetSurfaceFlowAt(const FVector2D& At) const = 0;

	/**
	 * Mass per unit volume of this body, in kg per cubic centimetre.
	 *
	 * What anything frozen out of it has to be lighter than in order to float.
	 * Nothing in the floating code names water or ice: it compares two densities,
	 * so a crust on lava and a floe on a pond are the same arithmetic.
	 */
	virtual float GetSurfaceDensity() const = 0;

	/**
	 * The BED under this body at a point -- how far down the bottom is.
	 *
	 * Where a slab too heavy to float comes to rest. A puddle's bed is the ground
	 * it formed on; a river's is the floor of its channel.
	 */
	virtual float GetSurfaceBedAt(const FVector2D& At) const = 0;
};
