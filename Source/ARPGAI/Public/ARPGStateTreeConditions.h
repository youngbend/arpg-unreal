// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeConditionBase.h"
#include "StateTreeExecutionContext.h"
#include "ARPGStateTreeConditions.generated.h"

class AActor;

/**
 * The domain conditions, as StateTree conditions.
 *
 * WHAT THE FRAMEWORK SWAP ACTUALLY FIXED. As behaviour-tree decorators these
 * were evaluated when their branch was entered and never again: none of the four
 * set FlowAbortMode or asked to be ticked, so "health below 30%" gated the entry
 * to a flee branch but could not interrupt an attack already in progress. The
 * class comment claimed they aborted lower-priority branches when they changed.
 * They did not. It was a real bug, and it was invisible because no tree asset
 * existed to exhibit it.
 *
 * A StateTree transition re-tests its conditions while its state is active, so
 * the interruption is the default rather than something each node has to opt
 * into and can silently forget. That property is the single biggest reason this
 * migration is worth its cost.
 *
 * Everything generic -- comparisons, gameplay-tag queries, distance checks --
 * ships with StateTree and is deliberately NOT ported. What is here is only what
 * reads THIS game's combat components.
 */

/** Shared by the conditions that ask something about the current target. */
USTRUCT()
struct ARPGAI_API FARPGStateTreeTargetConditionInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<AActor> Target = nullptr;
};

/**
 * True when the target is within the actor's own weapon reach.
 *
 * Reach comes from the equipped weapon rather than being authored here, so a
 * greatsword NPC engages from further out than a dagger one using the same tree
 * -- which is the whole reason a tree is shared across archetypes.
 */
USTRUCT(meta = (DisplayName = "ARPG Target In Attack Range", Category = "ARPG|Combat"))
struct ARPGAI_API FARPGStateTreeCondition_TargetInAttackRange : public FStateTreeConditionCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FARPGStateTreeTargetConditionInstanceData;

	virtual const UStruct* GetInstanceDataType() const override
	{
		return FInstanceDataType::StaticStruct();
	}

	/** Added to the weapon's own reach. */
	UPROPERTY(EditAnywhere, Category = "Range")
	float ReachTolerance = 50.f;

	virtual bool TestCondition(FStateTreeExecutionContext& Context) const override;
};

/**
 * True when the actor is already facing the target closely enough to commit.
 *
 * Paired with the range check to gate an attack, so the NPC lines up BEFORE
 * swinging rather than starting a swing while still turning. That matters most
 * with a slow, weighty attack rotation, where almost no reorientation happens
 * once the attack has begun.
 */
USTRUCT(meta = (DisplayName = "ARPG Facing Target", Category = "ARPG|Combat"))
struct ARPGAI_API FARPGStateTreeCondition_FacingTarget : public FStateTreeConditionCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FARPGStateTreeTargetConditionInstanceData;

	virtual const UStruct* GetInstanceDataType() const override
	{
		return FInstanceDataType::StaticStruct();
	}

	UPROPERTY(EditAnywhere, Category = "Facing",
		meta = (ClampMin = "0.0", ClampMax = "180.0"))
	float MaxAngleDegrees = 20.f;

	virtual bool TestCondition(FStateTreeExecutionContext& Context) const override;
};

USTRUCT()
struct ARPGAI_API FARPGStateTreeHealthConditionInstanceData
{
	GENERATED_BODY()
};

/** True when the actor's health fraction is below the threshold. */
USTRUCT(meta = (DisplayName = "ARPG Health Below", Category = "ARPG|Combat"))
struct ARPGAI_API FARPGStateTreeCondition_HealthBelow : public FStateTreeConditionCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FARPGStateTreeHealthConditionInstanceData;

	virtual const UStruct* GetInstanceDataType() const override
	{
		return FInstanceDataType::StaticStruct();
	}

	UPROPERTY(EditAnywhere, Category = "Health",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Threshold = 0.3f;

	virtual bool TestCondition(FStateTreeExecutionContext& Context) const override;
};

USTRUCT()
struct ARPGAI_API FARPGStateTreeLeashConditionInstanceData
{
	GENERATED_BODY()

	/** Bind to the perception evaluator's HomeLocation. */
	UPROPERTY(EditAnywhere, Category = "Input")
	FVector HomeLocation = FVector::ZeroVector;

	/**
	 * Per-INSTANCE, because one tree asset is shared by every NPC using it.
	 *
	 * A latch stored on the condition struct itself would be shared by every
	 * goblin in the level -- exactly the bug Godot's blackboard-keyed per-node
	 * state existed to avoid, and the same reason the behaviour-tree version used
	 * node memory. StateTree instance data is the same idea again.
	 */
	UPROPERTY()
	bool bLeashed = false;
};

/**
 * True when the actor has strayed further than LeashRange from home.
 *
 * ReleaseRange adds hysteresis: once leashed, the NPC stays leashed until it is
 * back inside the smaller radius. Without it an NPC hovering exactly at the
 * boundary flips between chasing and returning every tick, which reads as a
 * twitch rather than a decision.
 *
 * THE LATCH IS KEPT RATHER THAN EXPRESSED AS TWO STATES, which a state machine
 * could do natively -- enter Returning at LeashRange, leave it at ReleaseRange.
 * That is the more idiomatic StateTree answer and it is deliberately not taken:
 * it moves the hysteresis into an authored asset, where nothing enforces it and
 * a tree that forgets it simply twitches. Here the behaviour is a property of
 * the condition and survives whatever a designer wires around it.
 */
USTRUCT(meta = (DisplayName = "ARPG Is Leashed", Category = "ARPG|AI"))
struct ARPGAI_API FARPGStateTreeCondition_IsLeashed : public FStateTreeConditionCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FARPGStateTreeLeashConditionInstanceData;

	virtual const UStruct* GetInstanceDataType() const override
	{
		return FInstanceDataType::StaticStruct();
	}

	UPROPERTY(EditAnywhere, Category = "Leash", meta = (ClampMin = "0.0"))
	float LeashRange = 2500.f;

	/** 0 disables hysteresis, releasing at exactly LeashRange. */
	UPROPERTY(EditAnywhere, Category = "Leash", meta = (ClampMin = "0.0"))
	float ReleaseRange = 0.f;

	virtual bool TestCondition(FStateTreeExecutionContext& Context) const override;
};
