// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGVfxFittable.h"
#include "GameFramework/Actor.h"
#include "ARPGTestVfxActor.generated.h"

/**
 * A status visual that records the fitting handshake instead of drawing
 * anything.
 *
 * Exists because the interesting half of IARPGVfxFittable is what the subsystem
 * REFUSES to do to an implementer -- it must not also scale it, and it must
 * offer a fade-out rather than destroying it. Neither is observable from the
 * outside without something on the receiving end to say what it was handed.
 */
UCLASS()
class AARPGTestVfxActor : public AActor, public IARPGVfxFittable
{
	GENERATED_BODY()

public:
	AARPGTestVfxActor();

	//~ IARPGVfxFittable
	virtual void Fit_Implementation(FBox Bounds, int32 Stacks) override;
	virtual void SetStacks_Implementation(int32 Stacks) override;
	virtual void Finish_Implementation() override;
	//~ End IARPGVfxFittable

	/** Bounds handed over by the probe, in the target's local space. */
	UPROPERTY()
	FBox FittedBounds = FBox(ForceInit);

	UPROPERTY()
	bool bFitCalled = false;

	UPROPERTY()
	int32 LastStacks = 0;

	/** How many times the stack count was pushed without a respawn. */
	UPROPERTY()
	int32 SetStacksCalls = 0;

	/** Set instead of being destroyed, which is the whole point of Finish. */
	UPROPERTY()
	bool bFinishCalled = false;
};
