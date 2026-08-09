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

	/**
	 * Registers with the status VFX subsystem, and unregisters on the way out.
	 *
	 * HERE, rather than a per-actor component or a world sweep, because this is
	 * the one class everything with gameplay state already routes through --
	 * which makes the visual layer's discovery cost exactly zero authoring, the
	 * property worth preserving from the Godot original. That subsystem polls
	 * what is registered; see its class comment for why it polls.
	 *
	 * On INITIALIZE rather than BeginPlay, and the difference is real: an
	 * ability system can be carrying effects before play begins, and in a world
	 * with no game mode -- an automation fixture, a tools harness -- BeginPlay
	 * never runs at all while InitializeComponent always does. Registering
	 * something that turns out to have nothing on it costs one array entry.
	 */
	virtual void InitializeComponent() override;
	virtual void UninitializeComponent() override;
};
