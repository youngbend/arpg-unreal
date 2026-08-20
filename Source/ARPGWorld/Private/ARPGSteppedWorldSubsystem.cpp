// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGSteppedWorldSubsystem.h"
#include "ARPGWorldAuthority.h"
#include "Engine/World.h"

bool UARPGSteppedWorldSubsystem::HasAuthority() const
{
	return ARPGWorld::WorldHasAuthority(GetWorld());
}

float UARPGSteppedWorldSubsystem::GetStepInterval() const
{
	// The floor is the subclass's, not a constant: spread runs at 10Hz and
	// refuses to go below 1Hz, fluids run at 4Hz and tolerate 0.5Hz. Clamping
	// both to the same number would either forbid a legitimate slow rate or
	// allow spread to be configured into a busy loop.
	return 1.f / FMath::Max(GetMinStepRate(), GetStepRate());
}

void UARPGSteppedWorldSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!HasAuthority())
	{
		return;
	}

	StepAccumulator += DeltaTime;

	const float Interval = GetStepInterval();
	if (StepAccumulator < Interval)
	{
		return;
	}

	// The ACCUMULATED time, not the frame's -- see the class comment.
	StepSimulation(StepAccumulator);
	StepAccumulator = 0.f;
}
