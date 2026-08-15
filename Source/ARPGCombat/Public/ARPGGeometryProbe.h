// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGStatusEffectComponent.h"

class AActor;

/**
 * Measures how big a thing actually is, from whatever it happens to be made of.
 *
 * Shared by everything that FITS a visual to a target rather than asking an
 * author to hand-size it. That is the whole reason one authored fire works on a
 * rat, a tree and a barrel: what differs between them is the BOUNDS of the
 * thing alight, what differs between fire and corruption is the LOOK of the
 * medium, and those are orthogonal. Measure the first, author the second.
 */
namespace ARPGGeometryProbe
{
	/**
	 * Bounds of everything the actor is made of, in the actor's own LOCAL space.
	 *
	 * UNIONS rendered geometry AND collision, and the union is load-bearing
	 * rather than defensive. Preferring meshes and only falling back to
	 * colliders looks equivalent and is not: the moment a thing has any mesh at
	 * all its collision extent stops counting, and a body of water whose surface
	 * is a flat plane inside a taller box then measures as flat -- so everything
	 * asking how far the thing REACHES finds nothing. Reading colliders is
	 * equally load-bearing in the other direction: a prop whose visual is
	 * instanced elsewhere has a collision hull and nothing else.
	 *
	 * bVisualOnly restricts it to mesh components, for the one question the
	 * union answers badly: WHERE IS THIS THING'S SURFACE. Water's collider
	 * deliberately stands above the waterline so a spell arriving at the river
	 * enters it; asking the union for the surface puts effects in that headroom.
	 *
	 * Returns false when the actor describes no geometry at all, leaving
	 * OutBounds untouched.
	 */
	ARPGCOMBAT_API bool MeasureLocalBounds(const AActor* Target, FBox& OutBounds,
		bool bVisualOnly = false);

	/**
	 * Largest HORIZONTAL extent -- how wide the thing is.
	 *
	 * What radial effects want. Using the full diagonal would let a tall thin
	 * thing claim a footprint it does not have.
	 */
	ARPGCOMBAT_API double BoundsFootprint(const FBox& Bounds);

	/**
	 * Attaches Instance to Target, then places and sizes it against Target's
	 * measured geometry.
	 *
	 * MEASURES FIRST, THEN ATTACHES, and the order is not cosmetic. Attaching
	 * first makes the effect part of what is being measured -- a particle system
	 * carries generous fixed bounds, so a fitted effect inflates the bounds it
	 * is being fitted to, by more than the target's real size for anything
	 * small.
	 *
	 * An instance implementing IARPGVfxFittable is NOT scaled here: it has been
	 * handed the bounds and takes full responsibility for its own size. Scaling
	 * it as well would double-apply, and a particle system emitting in world
	 * space does not scale correctly through its parent transform anyway.
	 */
	ARPGCOMBAT_API void FitVfxToTarget(AActor* Instance, AActor* Target,
		EARPGStatusVfxFit FitMode, float ExtraScale, int32 Stacks);

	/**
	 * Where and how big a visual on this target should be, without touching
	 * anything.
	 *
	 * FITTING IS A MEASUREMENT, and it was tangled up with attaching an actor.
	 * A Niagara system attached straight to the target wants the same answer and
	 * has no actor to give it to -- so the sum lives here and the two callers
	 * differ only in what they apply it to.
	 *
	 * @return false when the target could not be measured; the outputs are then
	 *         the authored size unadjusted, which is what an unmeasurable target
	 *         should keep.
	 */
	ARPGCOMBAT_API bool SolveVfxFit(AActor* Target, EARPGStatusVfxFit FitMode,
		float ExtraScale, FVector& OutRelativeLocation, double& OutScale, FBox& OutBounds);

	/** The same fit, applied to a component already attached to the target. */
	ARPGCOMBAT_API void FitVfxComponent(USceneComponent* Instance, AActor* Target,
		EARPGStatusVfxFit FitMode, float ExtraScale);
}
