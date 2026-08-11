// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGSolidField.h"

class UDynamicMeshComponent;

/**
 * Ground-plane polygon maths shared by every part of the fluid system. Port of
 * Godot's fluid_geometry.
 *
 * A BODY OF FLUID IS A POLYGON IN THE XY PLANE, and that one decision is what
 * makes the whole system fall out of four library calls rather than bespoke
 * code: depositing is a union, rain is an outward offset, evaporation is an
 * inward offset, and freezing the overlap between a pool and a spell is
 * literally an intersection. Nothing is quantised to a grid, so a pool that has
 * had three spells land in it and an ice shard cut across it is a genuinely
 * irregular outline rather than a union of discs.
 *
 * (Godot's version worked in XZ because that was its ground plane; here it is
 * XY, with Z up. Same maths, different letter.)
 *
 * ONE RING, ALWAYS, for fluids. Every helper here returns a single simple outer
 * ring and callers store one. The boolean ops hand back holes and disjoint
 * islands; both are discarded in favour of the largest outer ring, which is not
 * a limitation but the physically right answer for a LIQUID -- cut a region out
 * of the middle of a puddle and the water flows back over it. Where the area
 * actually removed has to be conserved (lava and water making obsidian, where
 * the fluid really is used up) the caller follows the clip with ShrinkToArea,
 * which takes the ring back down uniformly.
 *
 * SOLIDS ARE THE EXCEPTION and keep their holes -- see SplitRingsAndHoles.
 */
namespace ARPGFluidGeometry
{
	/** Absolute area by the shoelace formula. Winding-agnostic. */
	ARPGWORLD_API double PolygonArea(const TArray<FVector2D>& Ring);

	/** Total edge length. The resize solver uses it to predict an offset's effect. */
	ARPGWORLD_API double PolygonPerimeter(const TArray<FVector2D>& Ring);

	/**
	 * Area-weighted centroid, falling back to the vertex average for a
	 * degenerate ring so callers always get a usable anchor point.
	 */
	ARPGWORLD_API FVector2D PolygonCentroid(const TArray<FVector2D>& Ring);

	/** Is this point inside the ring? Even-odd crossing test. */
	ARPGWORLD_API bool PolygonContains(const TArray<FVector2D>& Ring, const FVector2D& Point);

	/** Axis-aligned bounds, for a trigger box that only has to be generous. */
	ARPGWORLD_API FBox2D PolygonBounds(const TArray<FVector2D>& Ring);

	/**
	 * A regular n-gon inscribing the circle -- what a deposit is before it meets
	 * anything.
	 *
	 * Segments is the ONLY resolution knob in the whole system, and it governs
	 * how round a fresh, untouched puddle looks and nothing else: once a pool has
	 * been merged with or clipped, its outline comes from the boolean ops.
	 */
	ARPGWORLD_API TArray<FVector2D> MakeCircle(const FVector2D& Centre, double Radius, int32 Segments = 16);

	/**
	 * The stadium swept by a circle travelling from A to B -- the honest
	 * footprint of a capsule-shaped spell, which every forward-elongated
	 * discharge in this project uses.
	 */
	ARPGWORLD_API TArray<FVector2D> MakeStadium(const FVector2D& A, const FVector2D& B,
		double Radius, int32 Segments = 16);

	/**
	 * Union, keeping only the largest outer ring. Depositing.
	 *
	 * Islands are dropped rather than tracked: a splash that lands clear of a
	 * pool should become its own body, which is the SUBSYSTEM's decision, not
	 * something to smuggle back as a second ring on one pool.
	 */
	ARPGWORLD_API TArray<FVector2D> MergeRings(const TArray<FVector2D>& A, const TArray<FVector2D>& B);

	/** Offsets outward (positive) or inward (negative). Rain and evaporation. */
	ARPGWORLD_API TArray<FVector2D> OffsetRing(const TArray<FVector2D>& Ring, double Offset);

	/** Intersection, largest ring only. Freezing. */
	ARPGWORLD_API TArray<FVector2D> IntersectRings(const TArray<FVector2D>& A, const TArray<FVector2D>& B);

	/** Subtraction, largest ring only. What a pool loses to a solid. */
	ARPGWORLD_API TArray<FVector2D> SubtractRings(const TArray<FVector2D>& A, const TArray<FVector2D>& B);

	/**
	 * Intersection keeping the outer ring AND its holes, for a solid.
	 *
	 * HOLES ARE KEPT SEPARATE, and that is the whole point. Bridging them into
	 * the outline immediately -- which is what a solid needs in order to be
	 * DRAWN -- leaves a zero-width slit, and offsetting a ring that contains one
	 * makes the offsetter round the slit's ends into arcs. Erode it repeatedly
	 * and that compounds: the Godot version measured 50 vertices growing to 7193
	 * over eighteen melt ticks, turning a 0.1ms decomposition into 467ms -- a
	 * second-long stall every time a holed floe melted.
	 *
	 * Kept apart, each ring stays clean, and erosion is also physically right:
	 * the outer edge shrinks while the hole WIDENS.
	 */
	ARPGWORLD_API void IntersectWithHoles(const TArray<FVector2D>& A, const TArray<FVector2D>& B,
		TArray<FVector2D>& OutRing, TArray<TArray<FVector2D>>& OutHoles);

	/**
	 * Scales the ring about its centroid until it encloses the target area.
	 *
	 * Uniform rather than an inward offset, because an offset erodes thin necks
	 * away entirely and changes the SHAPE -- which is wrong when the point is
	 * only that some of the fluid was consumed. Solved directly: area scales
	 * with the square of a uniform scale, so this is one square root, not an
	 * iteration.
	 */
	ARPGWORLD_API TArray<FVector2D> ShrinkToArea(const TArray<FVector2D>& Ring, double TargetArea);

	/**
	 * The fifth call: turns an outline into something you can SEE.
	 *
	 * A slab -- a triangulated cap at TopZ, another at BottomZ, and walls joining
	 * them -- written straight onto a dynamic mesh component. Give it the same
	 * ring the body already stores and the drawn shape is the simulated shape by
	 * construction, with no second representation to fall out of step.
	 *
	 * The component takes it in LOCAL space, so Origin is the body's own XY
	 * centre; everything is emitted relative to that. UVs are still anchored to
	 * WORLD position, which matters: a pool's centroid moves every time it merges
	 * or erodes, and local UVs would make the whole surface texture swim sideways
	 * each tick while the water itself sat still.
	 *
	 * A hole is optional and is what a solid keeps -- see IntersectWithHoles. Pass
	 * an empty ring for a fluid, which never has one.
	 *
	 * TopZ == BottomZ is legal and gives a flat cap with no walls, which is what a
	 * body with no depth is.
	 */
	ARPGWORLD_API void BuildSlabMesh(UDynamicMeshComponent* Component,
		const TArray<FVector2D>& Ring, const TArray<FVector2D>& Hole,
		const FVector2D& Origin, double BottomZ, double TopZ);

	/**
	 * The sixth call, and the one with a CEILING: a heightfield as a mesh.
	 *
	 * Where BuildSlabMesh draws a polygon extruded to a flat thickness, this draws
	 * a field whose top and bottom vary per cell -- a floe with a bowl melted into
	 * it, a step where new ice froze at a lower waterline, a hole where the two
	 * surfaces met.
	 *
	 * WHY IT MATTERS FOR COST, and this is the whole reason a floe stopped being a
	 * polygon. A slab mesh is retriangulated from an outline that gains vertices
	 * every time it is clipped or offset, so a long-lived floe grows without
	 * bound -- the Godot version measured fifty vertices becoming seven thousand
	 * over eighteen melt ticks. A field cannot: its triangle count is at most a
	 * fixed few per cell, forever, no matter how many fireballs land on it.
	 *
	 * Emitted in the component's LOCAL space; the field's own heights already are.
	 */
	ARPGWORLD_API void BuildFieldMesh(UDynamicMeshComponent* Component,
		const FARPGSolidField& Field, const FVector2D& Origin);
}
