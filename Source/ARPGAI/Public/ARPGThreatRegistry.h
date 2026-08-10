// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectKey.h"
#include "ARPGThreatRegistry.generated.h"

class AActor;

/**
 * Who is currently being targeted by somebody, and by how many.
 *
 * WHY THIS EXISTS. "Is anything still interested in me" is the question the
 * player's auto-sheathe suppression asks, and it used to answer it by running
 * TActorIterator<AARPGAIController> over the entire level twice a second and
 * reading each controller's blackboard. That walks every actor in the world to
 * find a handful, and it gets worse exactly as the level grows.
 *
 * The answer is already known at the moment it changes -- an NPC acquiring or
 * losing a target -- so it is pushed here instead of being rediscovered. The
 * query becomes a map lookup.
 *
 * A COUNT, not a set of watchers. Two NPCs targeting the same player and one of
 * them disengaging must not clear the flag, and the count is the cheapest thing
 * that gets that right.
 */
UCLASS()
class ARPGAI_API UARPGThreatRegistry : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	/**
	 * Moves one watcher from OldTarget to NewTarget. Either may be null.
	 *
	 * Expressed as a transition rather than add/remove so a perception component
	 * cannot leak a count by forgetting one half of the pair.
	 */
	void NotifyTargetChanged(AActor* OldTarget, AActor* NewTarget);

	/** True when at least one NPC is currently committed to this actor. */
	UFUNCTION(BlueprintPure, Category = "ARPG|AI")
	bool IsTargeted(const AActor* Actor) const;

	/** How many are, for debug overlays and encounter pacing. */
	UFUNCTION(BlueprintPure, Category = "ARPG|AI")
	int32 GetThreatCount(const AActor* Actor) const;

private:
	TMap<FObjectKey, int32> ThreatCounts;
};
