// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "ARPGBTTask_AttemptParry.generated.h"

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
 * ONE SWING PER ATTEMPT. This does not rate-limit itself; wrap it in a cooldown
 * decorator so a target throwing a combo does not get every beat blocked. The
 * cooldown should arm on success only, so a whiff -- reacted too slowly, or the
 * swing turned out unblockable -- does not itself cost a window.
 */
UCLASS()
class ARPGAI_API UARPGBTTask_AttemptParry : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UARPGBTTask_AttemptParry();

	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector TargetKey;

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

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp,
		uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory,
		float DeltaSeconds) override;

	virtual void InitializeFromAsset(UBehaviorTree& Asset) override;
	virtual uint16 GetInstanceMemorySize() const override { return sizeof(FMemory); }
	virtual FString GetStaticDescription() const override;

	/**
	 * Time until the swing's hitbox reaches ContactRadius, from the current
	 * closing speed and acceleration. Negative when no prediction is possible.
	 *
	 * Public and static so the arithmetic is testable without staging an
	 * animated swing -- which is the only way to cover it at all.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|AI")
	static float PredictTimeToContact(float Distance, float ClosingSpeed, float Acceleration,
		float InContactRadius);

private:
	struct FMemory
	{
		/** Where the threshold landed for THIS attempt, jitter included. */
		float TriggerThreshold = 0.f;

		float PreviousDistance = -1.f;
		float PreviousClosingSpeed = 0.f;
		float SmoothedAcceleration = 0.f;

		bool bTriggered = false;
		float HoldRemaining = 0.f;
	};
};
