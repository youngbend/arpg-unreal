// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGSurfaceFacets.generated.h"

/**
 * How rough a slab's surface is drawn, over and above the outline it has.
 *
 * NOT A PROPERTY OF THE SHAPE. A slab is an outline extruded to a thickness, and
 * that is the truth every query answers from -- what you stand on, what you can
 * walk off, how much is left to break. This only decides how much the DRAWN
 * surface is allowed to wander off it, so two slabs with identical outlines can
 * read as ice and as rock.
 *
 * ZERO IS FLAT, and flat is honest for ice: a floe is a frozen water surface and
 * a water surface is level. Rock is not, and a pillar of earth drawn as a flat
 * extrusion reads as poured concrete -- so the two numbers below break its top up
 * and its edges in, before it is meshed and without the simulation ever knowing.
 *
 * IT COSTS TRIANGLES, which is the reason it is a choice rather than always on.
 * A flat slab is a triangulated outline -- a few dozen triangles for a whole floe
 * -- and relief needs a grid of interior vertices for the top to bend over.
 */
USTRUCT(BlueprintType)
struct ARPGWORLD_API FARPGSurfaceFacets
{
	GENERATED_BODY()

	// EditAnywhere rather than EditDefaultsOnly, on all of these, and it is the
	// containing property that decides where this is really editable -- the solid
	// definition holds it as EditDefaultsOnly and that is the gate that counts. A
	// member marked EditDefaultsOnly inside a struct is refused on any INSTANCE of
	// that struct, which includes the one the asset generator builds to hand to
	// set_editor_property, so the whole struct became unwritable from Python.

	/**
	 * How far the surface is broken up and down, in cm.
	 *
	 * THE WHOLE COLUMN MOVES, top and underside together, so the slab keeps
	 * exactly the thickness it was given -- the mesher lifts both caps by the same
	 * amount. Displacing them independently would thin the slab wherever the two
	 * happened to oppose.
	 *
	 * Keep it under about a fifth of the thickness. Past that a slab starts
	 * reading as rubble rather than as one piece of rock, and on something thin --
	 * a crust of obsidian -- it reads as holes.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Facets",
		meta = (ClampMin = "0.0"))
	float Relief = 0.f;

	/**
	 * How far apart the bumps are, in cm. Also what decides the tessellation.
	 *
	 * THE SURFACE IS SAMPLED, NOT HASHED. Vertices land wherever the triangulator
	 * puts them, and two of them can be a centimetre apart -- so a height taken
	 * straight from a hash of the position would give neighbours unrelated answers
	 * and the top would come out as spikes. A lattice this far apart, interpolated
	 * smoothly between its points, is what makes it read as rock instead.
	 *
	 * Interior vertices are placed at half this, so the cost of relief goes with
	 * the SQUARE of how fine it is. Big grain is cheap and boulder-like; small
	 * grain is expensive and gravelly.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Facets",
		meta = (ClampMin = "1.0"))
	float Grain = 60.f;

	/**
	 * How far the outline's own vertices are nudged, as a fraction of the edge
	 * they sit on.
	 *
	 * WHAT KEEPS A RAISED WALL FROM BEING A RECTANGLE. Relief roughens the top and
	 * leaves the silhouette perfectly straight, which from the side is still
	 * masonry. Nudging the vertices breaks the outline itself, so the shape reads
	 * as broken stone rather than as something cut.
	 *
	 * APPLIED ONCE, WHEN THE SLAB IS MADE, and then it IS the outline -- what you
	 * see and what you can stand on are the same polygon, as they are for
	 * everything else here. Kept well under a half because a vertex that crosses
	 * its neighbour turns the polygon inside out.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Facets",
		meta = (ClampMin = "0.0", ClampMax = "0.4"))
	float Spread = 0.f;
};
