// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGSolidField.generated.h"

/**
 * A slab of anything solid as a HEIGHTFIELD rather than an outline with a
 * thickness. Ice on water is the worked example; nothing here is about ice.
 *
 * WHY THIS REPLACED THE POLYGON, and it is not only about looks. A fluid is
 * genuinely two-dimensional -- a puddle has one surface and pouring more in makes
 * it wider, not deeper -- so a polygon is the honest model and every operation on
 * it is a boolean. A SLAB IS NOT. It has a top that can be melted at an angle, a
 * bottom that erodes separately, and a history: solidify over a floe a player is
 * standing on and the new material forms at the waterline the load pushed the
 * surface down to, which is a STEP no outline can hold.
 *
 * THREE PROBLEMS DISAPPEAR RATHER THAN BEING SOLVED.
 *
 *   HOLES STOP BEING THINGS. A hole is a cell whose top has met its bottom. It
 *   is not a ring, so it cannot be bridged into the outline, cannot leave a
 *   zero-width slit for the offsetter to round into arcs, and cannot be dropped
 *   on the way through because only the largest was kept. The Godot version's
 *   50 vertices becoming 7193 over eighteen melt ticks was that bridging; the
 *   holes that vanished mid-melt were that culling. Neither has anywhere to
 *   happen here.
 *
 *   COST BECOMES A CONSTANT. Melting is a write to the cells under the impact --
 *   no boolean, no offsetter, no retriangulation of an outline that grows a few
 *   vertices every time it is clipped. Triangle count is bounded by the grid
 *   forever, no matter how many fireballs land on it.
 *
 *   THE MELT SHAPE FALLS OUT. Subtracting a smooth falloff from the top gives a
 *   bowl; a bowl centred at the edge of a slab leaves the angled cut you would
 *   expect, and one deep enough opens a hole, from the same arithmetic with no
 *   case for either.
 *
 * SLAB-LOCAL Z. Heights are relative to the actor, not the world, so the whole
 * field rides up and down with the buoyancy without a single cell being touched.
 * Zero is where the underside of the slab started; the waterline sits at the
 * current draft, which is what makes "froze at a lower level" a fact the field
 * records rather than a special case someone has to write.
 */
USTRUCT(BlueprintType)
struct ARPGWORLD_API FARPGSolidField
{
	GENERATED_BODY()

	/** World XY of the centre of cell (0,0). Drifting moves THIS, not the cells. */
	UPROPERTY()
	FVector2D Origin = FVector2D::ZeroVector;

	UPROPERTY()
	float CellSize = 20.f;

	UPROPERTY()
	int32 CountX = 0;

	UPROPERTY()
	int32 CountY = 0;

	/**
	 * Top and bottom of the solid in each cell, in MILLIMETRES of slab-local Z.
	 *
	 * Integers because they replicate: a ten-metre floe at 20cm cells is 2500
	 * cells, and two floats each would be twenty kilobytes on the wire. Whole
	 * millimetres are finer than anything visible on a surface you walk on and
	 * halve that, and they make "did this change enough to be worth rebuilding"
	 * an integer comparison rather than an epsilon.
	 */
	UPROPERTY()
	TArray<int16> Top;

	UPROPERTY()
	TArray<int16> Bottom;

	bool IsValidField() const { return CountX > 0 && CountY > 0 && Top.Num() == CountX * CountY; }

	/**
	 * Recomputes the cached totals. Call after writing cells directly.
	 *
	 * WHY THEY ARE CACHED AT ALL. Area, volume, centroid and occupancy are each a
	 * full sweep of the grid, and the buoyancy tick wants all four EVERY FRAME --
	 * so a 2500-cell floe was doing ten thousand cell visits per frame to answer
	 * questions whose answers only change when something writes to it. Writing is
	 * rare and reading is constant, so the totals live with the data.
	 */
	void Refresh();

	/**
	 * Re-totals the film and drops any of it left on cells that are no longer
	 * solid. Called by Refresh, so melting cannot strand water over a hole.
	 */
	void RefreshWet();

	int32 Index(int32 X, int32 Y) const { return Y * CountX + X; }

	/** Cell containing a world XY, or (-1,-1) when it is off the grid. */
	FIntPoint CellAt(const FVector2D& World) const;

	/** World XY of a cell's centre. */
	FVector2D CentreOf(int32 X, int32 Y) const;

	/** Is this cell still solid -- has its top not yet met its bottom? */
	bool IsSolid(int32 X, int32 Y) const;

	/** Thickness in centimetres, or 0 where the slab has gone. */
	float ThicknessAt(int32 X, int32 Y) const;

	/** Solid thick enough to matter, at a world XY. What standing on it asks. */
	bool IsSolidAt(const FVector2D& World) const;

	/** Top of the slab at a world XY, in slab-local cm. */
	float TopAt(const FVector2D& World) const;

	/** Total plan area still carrying material, in square cm. */
	double SolidArea() const { return CachedArea; }

	/** Total volume of material, in cubic cm. What buoyancy weighs. */
	double SolidVolume() const { return CachedVolume; }

	/** Occupancy-weighted centre of what is left. */
	FVector2D SolidCentroid() const { return CachedCentroid; }

	/** How far the slab reaches from a point along a direction. */
	double SupportDistance(const FVector2D& From, const FVector2D& Direction) const;

	/** Cells still carrying material. Zero means the slab is gone. */
	int32 SolidCellCount() const { return CachedCells; }

	// --- The film running over it ----------------------------------------------
	//
	// MELTWATER HAS TO GET DOWN, and until now it did not: melting the top of an
	// ice tower produced water that appeared instantly in a puddle at the tower's
	// foot, because a fluid body has one flat height and the only height a slab
	// could name was its own base. The journey was missing entirely.
	//
	// THE GRID IS ALREADY HERE, which is the whole reason this is cheap. A shallow
	// water solver IS a heightfield with a depth per cell and an exchange rule
	// between neighbours -- the same object the slab has been since it stopped
	// being an outline. So the film is a third array over the two that exist, and
	// the flow is one sweep with no boolean ops, no offsetter and no outline to
	// retriangulate. Cost stays bounded by the cell count, exactly as melting is.
	//
	// WHAT THIS IS NOT is a fluid simulation of the world. It runs on a slab and
	// nowhere else, which happens to be the case worth having -- water off a
	// melting tower, lava off a softening pillar -- and it leaves the pool model
	// completely untouched. Fluid crossing open terrain would need pools to become
	// heightfields too, which is a rewrite rather than an addition.

	/**
	 * Depth of fluid lying on each cell, in CENTIMETRES above that cell's top.
	 *
	 * FLOATS AND NOT REPLICATED, unlike Top and Bottom, and both halves of that
	 * are deliberate. Not replicated -- by NotReplicated, which is the specifier
	 * that actually does it -- because a film is presentation -- what the
	 * film DOES that matters is arrive at the bottom, and what arrives is a pool,
	 * which replicates already. Floats because the integer millimetres that make
	 * Top affordable on the wire would quantise a two-millimetre film into
	 * nothing: every flow step would round its way to zero and the water would
	 * evaporate on the way down.
	 */
	UPROPERTY(NotReplicated, Transient)
	TArray<float> Wet;

	/** Fluid lying at a world XY, in cm. Zero everywhere dry. */
	float WetAt(const FVector2D& World) const;

	/** Total fluid on the slab, in cubic cm. */
	double WetVolume() const;

	/** Is there anything to flow? Cheap enough to gate a tick on. */
	bool HasWet() const { return Wet.Num() > 0 && CachedWet > 0.0; }

	/**
	 * Puts fluid onto the slab, spread over a disc.
	 *
	 * WHERE THE MELT HAPPENED, so the water starts at the bowl a fireball cut and
	 * runs from there. Fluid poured onto a cell that is not solid -- a hole melted
	 * clean through -- falls straight past and is returned rather than kept.
	 *
	 * @return the volume that found nowhere to land, in cubic cm.
	 */
	double Pour(const FVector2D& At, float Radius, double Volume);

	/**
	 * Runs the film downhill one step.
	 *
	 * THE PIPE MODEL, which is the standard shallow-water discretisation and is
	 * about as simple as a flow solver gets: a cell compares its own surface
	 * height against its four neighbours and gives volume to whichever are lower,
	 * in proportion to how much lower. Half the difference, so two cells trading
	 * across a step settle level instead of oscillating across it forever.
	 *
	 * DOUBLE BUFFERED, because otherwise a sweep in raster order runs faster
	 * downhill to the east than to the west -- the same cell's new depth would be
	 * read by the neighbour visited after it and not by the one visited before.
	 * Every cell reads the same snapshot.
	 *
	 * @param Rate how much of the available head moves per second.
	 * @param YieldSlope head in cm per cell below which nothing moves at all --
	 *        the half of viscosity a rate cannot express. Zero for water, which
	 *        runs off anything; high for lava, which stops on a slope and stands
	 *        thick because it needs more head before it will go.
	 * @param MinimumFilm below this a cell is dry. Without a floor the film
	 *        approaches zero asymptotically and the slab ticks forever.
	 * @param OutShedAt filled with the volume-weighted place it ran off, when any
	 *        did. Meaningless when the return is zero.
	 * @return volume that left the slab entirely, in cubic cm -- over an edge, or
	 *         through a hole. This is what becomes a puddle on the ground.
	 */
	double FlowStep(float DeltaTime, float Rate, float YieldSlope, float MinimumFilm,
		FVector2D& OutShedAt);

	// --- Writing ---------------------------------------------------------------

	/**
	 * Lays a grid over a polygon and fills it to a uniform thickness.
	 *
	 * How a floe begins: the frozen region is still found by clipping polygons,
	 * because THAT is a two-dimensional question about where two things overlapped.
	 * It is only what happens to the slab afterwards that wants a field.
	 */
	void BuildFrom(const TArray<FVector2D>& Ring, float InCellSize, float Thickness);

	/**
	 * Adds material wherever the region covers, with its top at SurfaceZ.
	 *
	 * THE LOWER LAYER. A floe carrying weight rides deeper, so its waterline in
	 * slab-local terms is higher up the slab than the original surface -- and new
	 * material tops out THERE, below what solidified when the floe was
	 * riding light. Take the weight off and the whole slab rises, step and all.
	 *
	 * @return true when any cell gained material.
	 */
	bool Resolidify(const TArray<FVector2D>& Ring, float SurfaceZ, float MinimumGain);

	/**
	 * Melts a bowl into the top, deepest at the centre and tapering to nothing at
	 * the rim.
	 *
	 * A BOWL RATHER THAN A CYLINDER, which is the whole reason a fireball at the
	 * edge of a floe cuts it at an angle instead of stamping a clean bite out of
	 * it. Where the bowl reaches the bottom the slab has gone and there is a hole,
	 * which is the same arithmetic and not a second case.
	 *
	 * @return volume removed, in cubic cm.
	 */
	double MeltBowl(const FVector2D& At, float Radius, float Depth);

	/**
	 * Thins every cell from both faces at once. Ambient warmth.
	 *
	 * @return volume removed.
	 */
	double MeltUniform(float FromTop, float FromBottom);

	/** Slides the whole field. Cells are untouched -- only where it sits changes. */
	void Translate(const FVector2D& Delta) { Origin += Delta; CachedCentroid += Delta; }

private:
	/**
	 * Totals, kept with the data rather than swept for on every read.
	 *
	 * NotReplicated AND Transient, and the first of those is the one that does the
	 * work. Transient governs saving to DISK; a UPROPERTY inside a replicated
	 * struct is in the network layout regardless, so marking these Transient alone
	 * sent every one of them -- including the film, which is a float per cell and
	 * doubled the heaviest payload in the system to say something the comment
	 * above it claimed was never sent.
	 *
	 * They are pure functions of the cells, so sending them would be sending the
	 * same information twice and inviting the two copies to disagree. A client
	 * rebuilds them when the field arrives.
	 */
	UPROPERTY(NotReplicated, Transient)
	double CachedArea = 0.0;

	UPROPERTY(NotReplicated, Transient)
	double CachedVolume = 0.0;

	UPROPERTY(NotReplicated, Transient)
	FVector2D CachedCentroid = FVector2D::ZeroVector;

	UPROPERTY(NotReplicated, Transient)
	int32 CachedCells = 0;

	/** Total film volume, so HasWet is a read rather than a sweep. */
	UPROPERTY(NotReplicated, Transient)
	double CachedWet = 0.0;
};
