// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "ARPGAbilitySystemComponent.generated.h"

/**
 * The project's ability system component.
 *
 * Thin for now -- it exists so that every actor in the game is already routing
 * through OUR type before any behaviour lands on it. Retrofitting a subclass
 * later means touching every spawn path, every Blueprint that references the
 * component, and every cast site; doing it up front costs nothing.
 *
 * Phase 2 hangs the STACK_EXTEND status-effect behaviour off here (GAS has no
 * native "add duration to the existing stack" policy), and phase 3 the poise
 * event routing.
 */
UCLASS()
class ARPGCOMBAT_API UARPGAbilitySystemComponent : public UAbilitySystemComponent
{
	GENERATED_BODY()

public:
	UARPGAbilitySystemComponent();
};
