// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ARPGAILibrary.generated.h"

/**
 * The AI's pure arithmetic, in a place that outlives whichever node calls it.
 *
 * WHY A LIBRARY RATHER THAN A STATIC ON THE NODE. Under behaviour trees this
 * lived on UARPGBTTask_AttemptParry as a static, and the parry test called it
 * there. A StateTree node is a USTRUCT, which cannot carry a UFUNCTION at all --
 * so keeping the arithmetic Blueprint-visible and independently testable means
 * moving it off the node. That it survived the framework swap untouched is the
 * argument for it having been separable in the first place.
 */
UCLASS()
class ARPGAI_API UARPGAILibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Time until a swing's hitbox reaches ContactRadius, from the current closing
	 * speed and acceleration. Negative when no prediction is possible.
	 *
	 * Pure and static so the arithmetic is testable without staging an animated
	 * swing -- which is the only way to cover it at all.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|AI")
	static float PredictTimeToContact(float Distance, float ClosingSpeed, float Acceleration,
		float ContactRadius);
};
