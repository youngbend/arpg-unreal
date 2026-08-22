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

	/**
	 * Distance from each cell's centre to the slab's OUTLINE, in millimetres,
	 * negative inside it. The sub-cell half of the shape.
	 *
	 * WITHOUT THIS A SLAB IS A STAIRCASE, and it is worth being exact about why,
	 * because the grid was never the thing at fault. Freezing computes the
	 * genuine overlap between the water's outline and the spell's -- see
	 * TrySolidify, whose own comment says that polygon is the entire reason a
	 * body is not a disc -- and BuildFrom then reduced it to one bit per cell by
	 * asking whether the cell's CENTRE was inside. Everything between two centres
	 * was lost, so a 2m floe at 20cm cells came out as a twenty-step staircase
	 * whatever it had actually frozen from.
	 *
	 * A DISTANCE INSTEAD OF A BIT. The zero crossing between two neighbouring
	 * cells says where the edge really ran, to a fraction of a cell, and the
	 * mesher cuts there -- so the drawn outline follows the water to well under a
	 * centimetre on a grid ten times coarser than that.
	 *
	 * CLAMPED TO ONE CELL EITHER SIDE, which is what keeps it affordable on the
	 * wire. Interpolation only ever looks one cell across, so a true distance
	 * further out than that says nothing the mesher can use -- and clamping turns
	 * the whole interior into one run and the whole exterior into another, which
	 * is exactly what the codec above wants. Only the band along the edge varies,
	 * and that is a perimeter's worth of cells rather than an area's.
	 *
	 * Empty is legal and means "no outline": every caller falls back to the cell
	 * rule, which is the behaviour this replaced.
	 */
	UPROPERTY()
	TArray<int16> Edge;

	bool IsValidField() const { return CountX > 0 && CountY > 0 && Top.Num() == CountX * CountY; }

	// --- Getting it across the wire ---------------------------------------------
	//
	// THE HEAVIEST THING THIS PROJECT REPLICATES, by an order of magnitude. A
	// ten-metre floe at 20cm cells is 2500 cells, and the default struct
	// serialiser sends every one of them -- 10KB -- whenever any part of the
	// struct changes. A melting floe changes four times a second.
	//
	// A DELTA WAS THE OBVIOUS ANSWER AND IT CANNOT BE MADE CORRECT HERE.
	// NetSerialize runs once per connection and is told nothing about which
	// connection it is serving, so it cannot know whether this client has prior
	// state to patch. A dirty span would be right for whoever was already
	// watching and would quietly corrupt anyone who joined, or relevanced in,
	// since the last change. Per-connection baselines are what NetDeltaSerialize
	// exists for, and that wants an array of identified items rather than a dense
	// grid.
	//
	// SO: COMPRESS INSTEAD OF DIFFING, which needs no baseline and is therefore
	// correct for every client by construction. Run-length encoding suits this
	// data almost perfectly, because the thing that makes a heightfield cheap to
	// melt is the same thing that makes it compressible -- most of a slab is at
	// exactly one height. A fresh slab is one run. A slab with a bowl in it is the
	// bowl plus a run either side of each affected row. Bottom is a single run for
	// anything that has never been melted from underneath, which is nearly
	// everything.
	//
	// Whole every time, so there is no ordering hazard, no baseline to lose, and
	// nothing that behaves differently for a late joiner.

	bool NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess);

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

	/** How far inside the outline this cell sits, in cm. Negative outside it. */
	float InsetAt(int32 X, int32 Y) const;

	/**
	 * WHERE THE SLAB ENDS, as one number per cell: positive where there is
	 * material, negative where there is not, and zero on the surface between.
	 *
	 * THE UNION OF TWO BOUNDARIES, taken as a minimum, which is all an
	 * intersection of two regions ever is. A slab stops either because the
	 * outline it was cut with ran out or because something melted through it, and
	 * whichever is nearer is the one you can see. Written as one scalar so the
	 * mesher has a single contour to follow rather than an outline and a hole
	 * rule that have to be kept from disagreeing at the corner where a fireball
	 * took a bite out of the rim.
	 *
	 * The melt half rides on the melt's own falloff -- MeltBowl leaves a smooth
	 * dish, so the thickness running out has a gradient to interpolate along
	 * rather than a cliff, and a hole comes out round.
	 */
	float SolidityAt(int32 X, int32 Y) const;

	/** The same, anywhere: bilinear between the four cell centres around a point. */
	float SolidityAtWorld(const FVector2D& World) const;

	/**
	 * Solid thick enough to matter, at a world XY. What standing on it asks.
	 *
	 * SUB-CELL, like the mesh. Answering from the containing cell alone made the
	 * thing you can stand on a different shape from the thing you can see -- by up
	 * to half a cell, which on an earth wall is 30cm of floor that either was not
	 * drawn or could not be walked on.
	 */
	bool IsSolidAt(const FVector2D& World) const;

	/**
	 * Top of the material at a cell, in slab-local cm, with a cell that has none
	 * borrowing from the neighbours that do.
	 *
	 * BECAUSE AN EMPTY CELL'S TOP IS A LIE. Where a slab has melted through, Top
	 * and Bottom are both parked at the height the two faces met -- the FLOOR of
	 * the hole -- and that value is the one thing an interpolating surface must
	 * not be allowed to read. Interpolate toward it and the rim of every hole
	 * slumps down into it, and the vertical edge a fresh cut ought to have becomes
	 * a bevel running out to nothing.
	 *
	 * Borrowing instead extrapolates the material FLAT across its own boundary, so
	 * the surface stays at full height right up to the point the silhouette cuts
	 * it off. The taper a melt genuinely has is still there, because MeltBowl put
	 * it in the cells that DO have material.
	 */
	float SurfaceTopAt(int32 X, int32 Y) const;

	/** The underside, by the same rule. */
	float SurfaceBottomAt(int32 X, int32 Y) const;

private:
	/** SurfaceTopAt and SurfaceBottomAt, which differ only in which plane. */
	float BorrowedAt(const TArray<int16>& Plane, int32 X, int32 Y) const;

	/**
	 * Re-derives Edge from which cells still have material.
	 *
	 * Needed after erosion takes a whole cell, because the cells behind the ones
	 * removed are the new rim and are still saturated at "deep inside". Quantised
	 * to half a cell, which is both where the boundary between a solid cell and an
	 * empty one runs and all the precision a one-cell band can hold.
	 */
	void RebuildBand();

public:

	/**
	 * Top of the slab at a world XY, in slab-local cm.
	 *
	 * BILINEAR, AND THAT IS NOT A REFINEMENT -- it is the same surface the mesher
	 * draws, evaluated by the same arithmetic at an arbitrary point. Answering
	 * from the containing cell alone made the height you stand at a staircase
	 * while the height you can see was a slope, and on a melted bowl those
	 * disagreed by a tenth of the slab's thickness.
	 */
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
	 *
	 * THE RING IS KEPT, as a distance rather than as a polygon -- see Edge. What
	 * the clip worked out is the shape the player was promised, and rounding it
	 * to whichever cell centres happened to fall inside threw away nearly all of
	 * it.
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

	/**
	 * Pulls the outline IN by a distance, taking the material it passes over.
	 *
	 * THE OTHER HALF OF MELTING, and for a long time there was no half at all.
	 * Ambient warmth thinned every cell equally and did nothing else, so a floe
	 * kept its exact plan while it got thinner and thinner -- and because every
	 * cell was the same thickness, every cell reached zero on the same tick. A
	 * floe did not shrink and vanish; it went from a full-size sheet to nothing
	 * between one frame and the next.
	 *
	 * A rim is exposed on its side as well as its faces, so it goes first. That
	 * is what makes a melting floe RETREAT -- and it is what makes it reach its
	 * minimum area, and be retired, while there is still thickness left to see.
	 *
	 * THIS IS ONLY POSSIBLE BECAUSE OF Edge. Moving an outline used to mean
	 * offsetting a polygon, which is the operation whose repeated application put
	 * fifty vertices at seven thousand -- see ARPGFluidGeometry::IntersectWithHoles.
	 * Here it is one addition per cell against a stored distance, and it cannot
	 * grow anything.
	 *
	 * @return volume removed, in cubic cm.
	 */
	double Erode(float Distance);

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

template<>
struct TStructOpsTypeTraits<FARPGSolidField> : public TStructOpsTypeTraitsBase2<FARPGSolidField>
{
	enum
	{
		WithNetSerializer = true
	};
};
