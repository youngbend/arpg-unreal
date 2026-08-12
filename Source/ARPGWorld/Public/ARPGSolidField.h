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
	 * Transient and rebuilt by Refresh: they are pure functions of the cells, so
	 * sending them would be sending the same information twice and inviting the
	 * two copies to disagree. A client rebuilds them when the field arrives.
	 */
	UPROPERTY(Transient)
	double CachedArea = 0.0;

	UPROPERTY(Transient)
	double CachedVolume = 0.0;

	UPROPERTY(Transient)
	FVector2D CachedCentroid = FVector2D::ZeroVector;

	UPROPERTY(Transient)
	int32 CachedCells = 0;
};
