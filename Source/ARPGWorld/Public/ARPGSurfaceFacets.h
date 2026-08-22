// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGSurfaceFacets.generated.h"

/**
 * How a heightfield is SKINNED -- how much of the grid underneath it the player
 * is allowed to see.
 *
 * NOT A PROPERTY OF THE FIELD, which is why this is its own struct rather than
 * four more members on FARPGSolidField. Two slabs with byte-identical cells
 * should be able to read as ice and as rock; what differs is entirely how the
 * mesher chooses to interpolate between the cells it was given, and none of it
 * is simulated, replicated, or asked about by anything that is not drawing.
 *
 * ZERO IS SMOOTH, and smooth is the honest reading of the data: heights sampled
 * at the corners between cells, interpolated across each cell, with the
 * silhouette cut wherever the material actually runs out. That is right for ice,
 * which froze out of a water surface and should reproduce its outline.
 *
 * ROCK IS NOT SMOOTH AND IS NOT A GRID EITHER. A pillar of earth drawn honestly
 * from a 40cm grid is a set of perfectly rectangular segments, which reads as
 * masonry rather than as stone -- so the two knobs below break the lattice up
 * before it is meshed. They displace the lattice, NOT the field: the simulation
 * still has the heights it had, and the same corner displaced by the same amount
 * every time means nothing swims when a spell rebuilds the mesh.
 */
USTRUCT(BlueprintType)
struct ARPGWORLD_API FARPGSurfaceFacets
{
	GENERATED_BODY()

	// EditAnywhere rather than EditDefaultsOnly, on all three, and it is the
	// containing property that decides where this is really editable -- the solid
	// definition holds it as EditDefaultsOnly and that is the gate that counts.
	// A member marked EditDefaultsOnly inside a struct is refused on any INSTANCE
	// of that struct, which includes the one the asset generator builds to hand
	// to set_editor_property, so the whole struct became unwritable from Python.

	/**
	 * How far the top of the slab is broken up and down, in cm.
	 *
	 * THE WHOLE COLUMN MOVES, top and bottom together, so a cell keeps exactly
	 * the thickness the simulation gave it. Displacing the two faces
	 * independently thins the slab wherever the two happened to oppose, and at a
	 * cell already down to its last centimetre that is a hole nobody melted.
	 *
	 * Keep it under about a third of the thickness. Past that a fresh slab reads
	 * as rubble rather than as one piece of rock.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Facets",
		meta = (ClampMin = "0.0"))
	float Relief = 0.f;

	/**
	 * How far the lattice corners slide sideways, as a fraction of a cell.
	 *
	 * WHAT ACTUALLY KILLS THE MINECRAFT LOOK, and it is worth more than Relief:
	 * relief alone gives a rectangular grid with a bumpy top, which still reads
	 * as a grid. Sliding the corners turns every quad into an irregular
	 * quadrilateral, so neither the top surface nor the silhouette has an
	 * axis-aligned edge left in it.
	 *
	 * Clamped below a half because a corner that crosses its neighbour turns the
	 * cell inside out.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Facets",
		meta = (ClampMin = "0.0", ClampMax = "0.45"))
	float Spread = 0.f;

	/**
	 * Hard normals: every triangle gets its own, so the surface reads as planes
	 * meeting at edges rather than as a curve.
	 *
	 * FALLS OUT OF NOT SHARING VERTICES rather than being computed separately.
	 * The mesher's smooth path reuses one vertex wherever cells meet, so the
	 * normal solver averages the faces around it; this path gives every triangle
	 * its own three, so each one's normal is its own face and nothing is averaged
	 * with anything. Costs vertices, which is why it is a choice.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Facets")
	bool bFlatShaded = false;
};
