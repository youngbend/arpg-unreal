// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGTestVfxActor.h"
#include "Components/SceneComponent.h"

AARPGTestVfxActor::AARPGTestVfxActor()
{
	PrimaryActorTick.bCanEverTick = false;

	// A bare scene root and nothing else. Anything with geometry would give the
	// actor bounds of its own, and the probe measures the TARGET -- an effect
	// that inflates what it is being fitted to is the exact bug the measure-then-
	// attach order exists to prevent, so the fixture must not mask it.
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

void AARPGTestVfxActor::Fit_Implementation(FBox Bounds, int32 Stacks)
{
	FittedBounds = Bounds;
	LastStacks = Stacks;
	bFitCalled = true;
}

void AARPGTestVfxActor::SetStacks_Implementation(int32 Stacks)
{
	LastStacks = Stacks;
	++SetStacksCalls;
}

void AARPGTestVfxActor::Finish_Implementation()
{
	bFinishCalled = true;
}
