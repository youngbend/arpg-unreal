// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "ARPGDamageGameplayEffect.generated.h"

/**
 * Instant GameplayEffect that runs UARPGDamageExecution.
 *
 * Configured in C++ rather than left as a Blueprint asset so the damage pipeline
 * is usable the moment the code compiles, with no content authoring step in
 * between. Blueprint subclasses can still be made for per-weapon tuning; they
 * inherit the execution wiring.
 */
UCLASS()
class ARPGCOMBAT_API UARPGDamageGameplayEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UARPGDamageGameplayEffect();
};
