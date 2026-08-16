// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ARPGNPCDefinition.h"
#include "ARPGPerceptionComponent.generated.h"

class AActor;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnTargetAcquired, AActor*, Target);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FARPGOnTargetLost);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnInvestigate, FVector, Location);

/**
 * Finds what an NPC should be fighting. Port of Godot's PerceptionComponent.
 *
 * IT PUBLISHES NOTHING AND IS READ INSTEAD. Under behaviour trees this wrote its
 * findings into a UBlackboardComponent every scan, which meant perception knew
 * the names of the keys a tree expected -- a contract enforced by nothing, where
 * a typo on either side produced a node that silently did nothing. StateTree has
 * no blackboard: FARPGStateTreeEvaluator_Perception reads these getters and
 * exposes them as bound properties, so the compiler now checks what a string
 * used to.
 *
 * WHY NOT UAIPerceptionComponent. The design's retention rule is that the sight
 * CONE gates acquisition but not retention -- an NPC mid-fight does not lose its
 * target because it turned its back for a moment. UE's sight sense drops a
 * target the moment it leaves the cone, so expressing this on top of it means
 * reimplementing retention anyway and then fighting the sense's own forget
 * timer. The senses here are ~150 lines, deterministic, and testable without a
 * perception system tick.
 *
 * THREE CHANNELS, DELIBERATELY DIFFERENT:
 *
 *   SIGHT     range + optional cone + optional line of sight. The cone gates
 *             ACQUISITION only, per above.
 *   HEARING   distance against the candidate's own current noise radius. Ignores
 *             range, cone and line of sight entirely: a loud enough sound is
 *             heard through a wall and from directly behind.
 *   DAMAGE    being hit. What it does depends on whether the attacker was
 *             already perceived -- see EARPGDamageReaction.
 *
 * An NPC already committed to a target ignores hits from anyone else. It never
 * abandons an active fight to chase a different attacker, which is what stops a
 * crowd fight turning into every NPC swapping targets each time they are grazed.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGAI_API UARPGPerceptionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGPerceptionComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Perception",
		meta = (ClampMin = "0.0"))
	float DetectionRange = 1200.f;

	/** Full width in degrees of the acquisition cone. 360 is omnidirectional. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Perception",
		meta = (ClampMin = "0.0", ClampMax = "360.0"))
	float FOVAngle = 360.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Perception")
	bool bRequireLineOfSight = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Perception",
		meta = (ClampMin = "0.0"))
	float EyeHeight = 160.f;

	/** Seconds a lost target is held before it is dropped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Perception",
		meta = (ClampMin = "0.0"))
	float MemoryDuration = 3.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Perception")
	EARPGDamageReaction DamageReaction = EARPGDamageReaction::Investigate;

	/**
	 * Seconds between scans. Perception does not need to run every frame.
	 *
	 * Drives the component's own tick interval, so the engine skips the dispatch
	 * rather than this entering the function to find nothing to do.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Perception",
		meta = (ClampMin = "0.0"))
	float ScanInterval = 0.2f;

	UFUNCTION(BlueprintPure, Category = "ARPG|Perception")
	AActor* GetTarget() const { return Target; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Perception")
	bool IsAlerted() const { return bAlerted; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Perception")
	FVector GetLastKnownLocation() const { return LastKnownLocation; }

	/**
	 * Where the last unattributed blow came from, for the investigate reaction.
	 *
	 * Deliberately NOT cleared when a target is acquired: an NPC that investigates,
	 * finds someone, fights and loses them should go back to the place that
	 * started it rather than to nowhere.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Perception")
	FVector GetInvestigateLocation() const { return InvestigateLocation; }

	/** True once anything has been written to the investigate location. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Perception")
	bool HasInvestigateLocation() const { return bHasInvestigateLocation; }

	/** Runs one scan immediately. Called by the tick, and directly by tests. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Perception")
	void Scan(float DeltaTime);

	/**
	 * Report a hit. Called by whatever notices the owner taking damage; the
	 * reaction depends on whether the attacker was already perceived.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Perception")
	void NotifyDamagedBy(AActor* Attacker, FVector FromLocation);

	/** Force a target on, bypassing every sense. For scripted encounters. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Perception")
	void SetTarget(AActor* NewTarget);

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Perception")
	FARPGOnTargetAcquired OnTargetAcquired;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Perception")
	FARPGOnTargetLost OnTargetLost;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Perception")
	FARPGOnInvestigate OnInvestigate;

	// --- Individual senses, public so each is independently testable ----------

	/** Within range, within the cone if one is set, and visible if required. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Perception")
	bool CanSee(AActor* Candidate, bool bApplyFOV) const;

	/** Loud enough right now to be heard at this distance, wall or no wall. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Perception")
	bool CanHear(AActor* Candidate) const;

private:
	bool HasLineOfSight(const AActor* Candidate) const;

	/** Everything hostile and alive within the detection radius. */
	void GatherCandidates(TArray<AActor*>& OutCandidates) const;

	/** False once a candidate carries State.Dead. No ability system means alive. */
	static bool IsAlive(const AActor* Candidate);

	/**
	 * Tells the world's threat registry this NPC moved its attention.
	 *
	 * Pushed rather than polled: the player's auto-sheathe used to answer "is
	 * anything targeting me" by iterating every actor in the level.
	 */
	void PublishTargetChange(AActor* OldTarget, AActor* NewTarget) const;

	UPROPERTY(Transient)
	TObjectPtr<AActor> Target;

	FVector LastKnownLocation = FVector::ZeroVector;
	FVector InvestigateLocation = FVector::ZeroVector;

	bool bAlerted = false;
	bool bHasInvestigateLocation = false;

	/** Counts down once the target stops being perceived. */
	float MemoryTimer = 0.f;
};
