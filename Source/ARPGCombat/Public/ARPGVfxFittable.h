// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ARPGVfxFittable.generated.h"

UINTERFACE(MinimalAPI, Blueprintable)
class UARPGVfxFittable : public UInterface
{
	GENERATED_BODY()
};

/**
 * A visual that adapts itself to whatever it was spawned on.
 *
 * Optional. A visual that does not implement this is positioned and uniformly
 * scaled to the target's measured bounds, which is enough for most effects and
 * needs no code at all. Implement it when uniform scale is the wrong answer --
 * emission extents, particle counts, light range, a mesh that should stretch
 * rather than grow.
 *
 * IMPLEMENTING THIS MEANS YOU OWN YOUR SIZE. Nothing scales an implementer:
 * it has been handed the bounds and is expected to act on them. Scaling it as
 * well would double-apply, and a particle system emitting in world space does
 * not scale correctly through its parent transform in any case.
 */
class ARPGCOMBAT_API IARPGVfxFittable
{
	GENERATED_BODY()

public:
	/**
	 * Called once on spawn with the target's bounds in the target's own local
	 * space -- which is also this actor's parent space, so the numbers can be
	 * used directly.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "ARPG|VFX")
	void Fit(FBox Bounds, int32 Stacks);

	/**
	 * Called when the stack count changes WITHOUT respawning the visual, so a
	 * fire can grow with the burn rather than restarting each time it is fed.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "ARPG|VFX")
	void SetStacks(int32 Stacks);

	/**
	 * Called instead of destroying the actor, so particles already in flight can
	 * finish rather than vanishing mid-air. The implementer is responsible for
	 * destroying itself once it has faded.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "ARPG|VFX")
	void Finish();
};
