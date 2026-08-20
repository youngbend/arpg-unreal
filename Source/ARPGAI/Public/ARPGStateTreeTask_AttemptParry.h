// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeTaskBase.h"
#include "StateTreeExecutionContext.h"
#include "ARPGStateTreeTask_AttemptParry.generated.h"

class AActor;
class UARPGParryComponent;

/**
 * Per-ATTEMPT state. One tree asset is shared by every NPC using it, so a
 * threshold or a distance history stored on the node struct would be shared by
 * every goblin in the level.
 */
USTRUCT()
struct ARPGAI_API FARPGStateTreeAttemptParryInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<AActor> Target = nullptr;

	/**
	 * This pawn's guard, resolved once when the attempt starts.
	 *
	 * Cached rather than looked up again each tick: Tick ran
	 * FindComponentByClass on every frame of every attempt, which is a walk of
	 * the whole component array to re-find something that cannot have changed
	 * for the life of the attempt. EnterState already had to find it to decide
	 * whether the attempt was possible at all.
	 */
	UPROPERTY()
	TObjectPtr<UARPGParryComponent> Parry = nullptr;

	/** Where the threshold landed for THIS attempt, jitter included. */
	UPROPERTY()
	float TriggerThreshold = 0.f;

	UPROPERTY()
	float PreviousDistance = -1.f;

	UPROPERTY()
	float PreviousClosingSpeed = 0.f;

	UPROPERTY()
	float SmoothedAcceleration = 0.f;

	UPROPERTY()
	bool bTriggered = false;

	UPROPERTY()
	float HoldRemaining = 0.f;
};

/**
 * Watches the target for an incoming swing and times a block against it. Port of
 * Godot's BTAttemptParry, the most involved node in the AI.
 *
 * TIMED OFF REAL GEOMETRY, NOT A GUESS. The primary signal is the target's live
 * weapon hitbox position -- a real component riding the animated hand bone --
 * from which this tracks closing distance and solves kinematically for time to
 * contact. That is accurate regardless of weapon length or character size,
 * which a flat authored lead time is not: the same constant that parries a
 * dagger correctly is hundreds of milliseconds wrong for a greatsword.
 *
 * THE MONTAGE IS THE FALLBACK, and this is where phase 4 pays off. When there is
 * no usable hitbox motion yet, the remaining time is read from the target's
 * ACTIVE MONTAGE -- time left in Windup plus the lead into Active. Godot could
 * not do this: its AttackDefinition stored clip NAMES with no access to
 * durations, so animation.gd had to publish phase timings every frame purely so
 * the AI could read them. A montage knows its own section times, so that whole
 * channel disappears.
 *
 * THE TRIGGER THRESHOLD IS DERIVED, NOT AUTHORED. It is read from this actor's
 * own parry component each attempt -- BlendInTime + ParryWindow/2 -- which aims
 * the CENTRE of the perfect-parry window at predicted contact, leaving equal
 * slack for prediction error in both directions. Guard timings are therefore
 * never duplicated here as a second set of knobs that can drift.
 *
 * ONE SWING PER ATTEMPT. This does not rate-limit itself. Put the state behind a
 * cooldown so a target throwing a combo does not get every beat blocked, and arm
 * that cooldown on success only -- a whiff (reacted too slowly, or the swing
 * turned out unblockable) should not itself cost a window.
 */
USTRUCT(meta = (DisplayName = "ARPG Attempt Parry", Category = "ARPG|Combat"))
struct ARPGAI_API FARPGStateTreeTask_AttemptParry : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	FARPGStateTreeTask_AttemptParry();

	using FInstanceDataType = FARPGStateTreeAttemptParryInstanceData;

	virtual const UStruct* GetInstanceDataType() const override
	{
		return FInstanceDataType::StaticStruct();
	}

	/**
	 * EXTRA lead on top of the derived threshold. 0 aims dead centre; positive
	 * reacts earlier, biasing toward "window waning as the hit lands" but safer
	 * against a late prediction.
	 *
	 * This and ReactionJitter are the intended parry-skill tuning surface.
	 */
	UPROPERTY(EditAnywhere, Category = "Reaction", meta = (ClampMin = "0.0"))
	float ReactionDelay = 0.15f;

	/**
	 * Standard deviation of Gaussian noise on ReactionDelay, per attempt.
	 *
	 * 0 is perfectly consistent reflexes, which reads as robotic: every attempt
	 * either always parries or always fails. A small spread gives some perfect
	 * parries, some plain blocks and some clean hits from the same NPC -- raise
	 * it to make good parries rarer without touching the mean.
	 */
	UPROPERTY(EditAnywhere, Category = "Reaction", meta = (ClampMin = "0.0"))
	float ReactionJitter = 0.05f;

	/** Extra seconds to hold the guard past the window, absorbing timing slop. */
	UPROPERTY(EditAnywhere, Category = "Reaction", meta = (ClampMin = "0.0"))
	float HoldBuffer = 0.10f;

	/**
	 * How close the hitbox has to get to count as contact -- roughly the
	 * weapon's own collision size plus the defender's body.
	 *
	 * A single constant rather than per-weapon data, because no such data
	 * exists to read. Tune if a specific matchup consistently reacts early.
	 */
	UPROPERTY(EditAnywhere, Category = "Prediction", meta = (ClampMin = "0.0"))
	float ContactRadius = 60.f;

	/**
	 * Scales the TIME window the prediction is compared against, not the
	 * prediction itself.
	 *
	 * Deliberately this way round: an earlier Godot version scaled a
	 * distance-over-time extrapolation directly, which made the acceleration
	 * term's noise sensitivity grow with the horizon being tested -- so widening
	 * the lead for one weapon made an unrelated weapon fire absurdly early.
	 */
	UPROPERTY(EditAnywhere, Category = "Prediction", meta = (ClampMin = "0.1"))
	float ClosingSpeedMargin = 1.f;

	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context,
		const FStateTreeTransitionResult& Transition) const override;
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context,
		const float DeltaTime) const override;
};
