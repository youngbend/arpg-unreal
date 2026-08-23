// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

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

	/**
	 * Distance from a point to the ring's nearest edge, negative inside it.
	 *
	 * HOW FAR OFF THE SLAB A POINT IS, which a body made of a polygon can answer
	 * exactly -- see AARPGSolidBody::DistanceToEdge, which is this and nothing
	 * else. To the nearest SEGMENT rather than the nearest vertex: a ring's
	 * vertices are metres apart on a long river bank and centimetres apart where a
	 * spell clipped it, and a vertex-only distance would call a point resting
	 * against the first of those far outside.
	 *
	 * Inside-ness comes from PolygonContains rather than from a winding test of
	 * its own, so the sign here and the answer there cannot disagree about a
	 * point sitting on the line.
	 */
	ARPGWORLD_API double PolygonSignedDistance(const TArray<FVector2D>& Ring,
		const FVector2D& Point);

	/**
	 * The point ON the ring nearest a given one, or the point itself when inside.
	 *
	 * WHERE SOMETHING ACTUALLY HIT. A spell's volume is a sphere and a body is a
	 * polygon, so the two meet while the sphere's CENTRE is still outside -- it
	 * touched the rim, and the rim is where the contact is. Taking the centre
	 * unmodified puts the event beside the thing it struck, which for anything
	 * that then carves, deposits or scorches at that point is a spell landing
	 * where it visibly did not.
	 */
	ARPGWORLD_API FVector2D ClosestPointOnPolygon(const TArray<FVector2D>& Ring,
		const FVector2D& Point);

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
	 * Subtraction keeping the outer ring AND any hole the cut opened.
	 *
	 * WHAT A BITE OUT OF THE MIDDLE ACTUALLY IS. SubtractRings keeps the largest
	 * outer ring and throws the rest away, which is right for a liquid -- water
	 * flows back over a gap cut in it -- and wrong for anything solid: a fireball
	 * landing in the middle of an earth wall takes a piece OUT, and dropping the
	 * hole means the wall is drawn untouched and the spell did nothing visible.
	 *
	 * Also what freezing does to the water it takes: ice occupies the region it
	 * froze from, so the pool genuinely loses that ground rather than shrinking
	 * uniformly somewhere else.
	 */
	ARPGWORLD_API void SubtractWithHoles(const TArray<FVector2D>& A, const TArray<FVector2D>& B,
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
	 * The same scale outward, for fluid ADDED to a body rather than taken from it.
	 *
	 * WHY NOT JUST MERGE A CIRCLE IN. Because a puddle is a polygon with no volume
	 * of its own, and a circle that lands inside the outline unions to exactly the
	 * outline it landed in -- so water poured into the middle of a puddle would
	 * vanish. Growing the whole ring is the only way the volume survives, and it
	 * is the same arithmetic as consumption run backwards.
	 *
	 * Refuses to shrink, so the pair keep one-way contracts and a caller cannot
	 * quietly get the opposite of what it asked for.
	 */
	ARPGWORLD_API TArray<FVector2D> GrowToArea(const TArray<FVector2D>& Ring, double TargetArea);

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
	/**
	 * How high the floor is under a point, for a body that follows it.
	 *
	 * Local Z in the body's own frame, so zero is the height the body was laid
	 * at. Unset means FLAT, which is what a body on level ground is and what
	 * every caller wanted before floors had slopes in them.
	 */
	using FBedSampler = TFunction<double(const FVector2D& World)>;

	// THE TOP ON ITS OWN. Bed moves the whole body, both caps together, because a
	// puddle following a ramp is a sheet down the ramp rather than a wedge. A
	// TopRelief moves only the upper surface, which is how a slab gets a DISH cut
	// into it -- the underside stays where it was and the slab is genuinely
	// thinner where it was struck.
	//
	// Both are sampled per vertex, so what they can express is limited only by how
	// finely the cap is tessellated -- see DetailSpacing. Nothing about this shape
	// is a prism; it was only ever drawn as one.

	ARPGWORLD_API void BuildSlabMesh(UDynamicMeshComponent* Component,
		const TArray<FVector2D>& Ring, const TArray<FVector2D>& Hole,
		const FVector2D& Origin, double BottomZ, double TopZ,
		const FBedSampler& Bed = FBedSampler(), double DetailSpacing = 0.0,
		const FBedSampler& TopRelief = FBedSampler());

}
