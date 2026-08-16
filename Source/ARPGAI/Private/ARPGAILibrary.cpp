// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGAILibrary.h"

float UARPGAILibrary::PredictTimeToContact(float Distance, float ClosingSpeed, float Acceleration,
	float ContactRadius)
{
	const float RemainingDistance = FMath::Max(0.f, Distance - ContactRadius);

	if (RemainingDistance <= 0.f)
	{
		return 0.f; // already touching
	}

	// Effectively constant speed: a plain linear solve, and no acceleration term
	// to amplify sampling noise.
	if (FMath::Abs(Acceleration) < 1.f)
	{
		return ClosingSpeed > KINDA_SMALL_NUMBER ? RemainingDistance / ClosingSpeed : -1.f;
	}

	// d = v*t + a*t^2/2, solved for t.
	const float Discriminant = ClosingSpeed * ClosingSpeed + 2.f * Acceleration * RemainingDistance;
	if (Discriminant < 0.f)
	{
		// At the current (decelerating) rate this approach never reaches contact
		// at all. Correctly no prediction rather than a guess.
		return -1.f;
	}

	const float Root = FMath::Sqrt(Discriminant);
	const float First = (-ClosingSpeed + Root) / Acceleration;
	const float Second = (-ClosingSpeed - Root) / Acceleration;

	// The smallest non-negative root: the FIRST time contact is reached.
	float Best = -1.f;
	if (First >= 0.f)
	{
		Best = First;
	}
	if (Second >= 0.f && (Best < 0.f || Second < Best))
	{
		Best = Second;
	}

	return Best;
}
