// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ARPGSteppedWorldSubsystem.generated.h"

/**
 * Shared base for the elemental solvers: server-authoritative, and advanced in
 * fixed steps rather than per frame.
 *
 * WHY IT EXISTS. All four solvers answered the same two questions and answered
 * them separately. HasAuthority() was written out four times -- three
 * byte-identical bodies with three different comments, plus one delegating to a
 * file-local helper nothing else could reach. The fixed-step accumulator was
 * written out twice, in spread and fluids, as the same six lines. A fifth solver
 * would have copied whichever it was pasted next to.
 *
 * AUTHORITY IS NOT OPTIONAL HERE. A world subsystem ticks and receives calls on
 * clients as well as the server, and none of these solvers replicate anything:
 * an ungated client runs its own divergent copy of world state and then applies
 * gameplay effects and consumes invincibility frames from it. A world with no
 * net driver -- an automation fixture -- counts as authoritative, because there
 * is nobody else to be.
 *
 * FIXED STEPS, NOT FRAMES. Diffusion, evaporation and freezing are slow physical
 * processes; running them per frame makes fire spread faster on a faster machine
 * and charges every polygon rebuild to the render frame. Subclasses declare a
 * rate and receive the ACCUMULATED time, so the simulation advances in real
 * seconds whatever the frame rate.
 *
 * GetStatId() stays with each subclass on purpose. The macro names the stat
 * after the class, and collapsing four solvers into one profiler line would
 * throw away exactly the information a profile of them is for.
 */
UCLASS(Abstract)
class ARPGWORLD_API UARPGSteppedWorldSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** True when this subsystem may mutate authoritative world state. */
	UFUNCTION(BlueprintPure, Category = "ARPG|World")
	bool HasAuthority() const;

	//~ FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	//~ End FTickableGameObject

protected:
	/**
	 * Simulation steps per second. Clamped against a floor by GetStepInterval so
	 * a zero from config cannot divide by zero or busy-step.
	 */
	virtual float GetStepRate() const PURE_VIRTUAL(UARPGSteppedWorldSubsystem::GetStepRate, return 10.f;);

	/** Lowest rate a subclass will be held to. Fluids run slower than spread. */
	virtual float GetMinStepRate() const { return 1.f; }

	/** Seconds between steps. */
	float GetStepInterval() const;

	/**
	 * One fixed step. DeltaTime is the ACCUMULATED time since the last step, not
	 * the frame's -- see the class comment.
	 *
	 * Called only on authority, and only once enough time has built up.
	 */
	virtual void StepSimulation(float DeltaTime) PURE_VIRTUAL(UARPGSteppedWorldSubsystem::StepSimulation, );

	/**
	 * Discards accumulated time. For a subsystem that has just been reset or
	 * reconfigured and should not immediately fire a catch-up step.
	 */
	void ResetStepAccumulator() { StepAccumulator = 0.f; }

private:
	float StepAccumulator = 0.f;
};
