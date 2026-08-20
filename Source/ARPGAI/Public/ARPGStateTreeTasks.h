// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeTaskBase.h"
#include "StateTreeExecutionContext.h"
#include "ARPGWeaponAttackTree.h"
#include "ARPGStateTreeTasks.generated.h"

class AActor;
class UEnvQuery;

/**
 * The domain actions, as StateTree tasks.
 *
 * Movement, waiting and property manipulation are already engine tasks and are
 * deliberately not ported -- what remains is only the part that drives THIS
 * game's combat components, plus the two that now delegate their movement and
 * rotation to the engine rather than doing it by hand.
 */

USTRUCT()
struct ARPGAI_API FARPGStateTreeAttackInstanceData
{
	GENERATED_BODY()
};

/**
 * Throws one attack through the combo component.
 *
 * Succeeds on the PRESS, not on the swing landing: the combo component owns the
 * beat, and a task that ran until the montage finished would hold the state
 * active and stop its transitions being taken -- which is exactly when a
 * reactive parry or a flee needs to interrupt.
 */
USTRUCT(meta = (DisplayName = "ARPG Attack", Category = "ARPG|Combat"))
struct ARPGAI_API FARPGStateTreeTask_Attack : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FARPGStateTreeAttackInstanceData;

	virtual const UStruct* GetInstanceDataType() const override
	{
		return FInstanceDataType::StaticStruct();
	}

	/** Which input this beat represents, so a tree can author heavy openers. */
	UPROPERTY(EditAnywhere, Category = "Attack")
	EARPGAttackInput Input = EARPGAttackInput::Light;

	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context,
		const FStateTreeTransitionResult& Transition) const override;
};

USTRUCT()
struct ARPGAI_API FARPGStateTreeFaceTargetInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<AActor> Target = nullptr;
};

/**
 * Turns to face the target, finishing once within tolerance.
 *
 * NOW THE ENGINE'S FOCUS SYSTEM, NOT A HAND-ROLLED TURN. The behaviour-tree
 * version interpolated a yaw itself and called SetActorRotation every tick,
 * which wrote rotation straight past the movement component: it fought
 * bOrientRotationToMovement, bypassed the movement component's rotation
 * replication, and stomped any rotation coming out of an attack montage's root
 * motion. This sets AAIController focus instead and lets
 * UCharacterMovementComponent::RotationRate do the turning, which is the same
 * path the engine's own UBTTask_RotateToFaceBBEntry takes.
 *
 * The turn stays visibly gradual -- the tell a player reads to decide whether to
 * close or back off -- but the RATE now lives on the movement component, set
 * from the archetype's TurnRateDegrees. That is the better home for it by the
 * same argument that puts LeashRange on the definition: how fast a creature
 * pivots is a property of the creature, not of one tree node.
 */
USTRUCT(meta = (DisplayName = "ARPG Face Target", Category = "ARPG|AI"))
struct ARPGAI_API FARPGStateTreeTask_FaceTarget : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	FARPGStateTreeTask_FaceTarget();

	using FInstanceDataType = FARPGStateTreeFaceTargetInstanceData;

	virtual const UStruct* GetInstanceDataType() const override
	{
		return FInstanceDataType::StaticStruct();
	}

	UPROPERTY(EditAnywhere, Category = "Facing",
		meta = (ClampMin = "0.0", ClampMax = "180.0"))
	float ToleranceDegrees = 5.f;

	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context,
		const FStateTreeTransitionResult& Transition) const override;
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context,
		const float DeltaTime) const override;
	virtual void ExitState(FStateTreeExecutionContext& Context,
		const FStateTreeTransitionResult& Transition) const override;
};

USTRUCT()
struct ARPGAI_API FARPGStateTreeCreateSpaceInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<AActor> Target = nullptr;

	UPROPERTY()
	float Elapsed = 0.f;

	/** Counts up since the last retreat point was chosen. */
	UPROPERTY()
	float SinceDecision = 0.f;

	UPROPERTY()
	bool bRequestedMove = false;
};

/**
 * Retreats from the target until far enough, for backing off or fleeing.
 *
 * NOW NAVMESH-AWARE, WHICH IS A BUG FIX AND NOT ONLY A TIDY-UP. The
 * behaviour-tree version pushed AddMovementInput along the away-vector with no
 * navigation query at all, so it happily drove the NPC into walls, off ledges,
 * and into the very corner its own comment claimed a direction rather than a
 * destination would avoid. A direction does not avoid the corner; it just fails
 * there quietly until the timeout fires and reports success.
 *
 * This samples reachable points on the navmesh and moves to the one furthest
 * from the target, which is what an EQS donut query does and what
 * AAIController::MoveToLocation was built to execute. In a corner it now finds
 * the sideways escape instead of grinding into geometry.
 *
 * RetreatQuery is the designer-authored upgrade path: an EQS asset can score
 * cover, elevation and line of sight in ways this sampler deliberately does not
 * try to. See the note in the .cpp -- the hook is here, the execution is not
 * wired, because an EQS asset cannot be authored outside the editor and shipping
 * an untested query path would be worse than an honest gap.
 */
USTRUCT(meta = (DisplayName = "ARPG Create Space", Category = "ARPG|AI"))
struct ARPGAI_API FARPGStateTreeTask_CreateSpace : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	FARPGStateTreeTask_CreateSpace();

	using FInstanceDataType = FARPGStateTreeCreateSpaceInstanceData;

	virtual const UStruct* GetInstanceDataType() const override
	{
		return FInstanceDataType::StaticStruct();
	}

	UPROPERTY(EditAnywhere, Category = "Movement", meta = (ClampMin = "0.0"))
	float DesiredDistance = 600.f;

	/** Gives up rather than backing away forever into terrain. */
	UPROPERTY(EditAnywhere, Category = "Movement", meta = (ClampMin = "0.1"))
	float Timeout = 4.f;

	UPROPERTY(EditAnywhere, Category = "Movement", meta = (ClampMin = "0.0"))
	float AcceptanceRadius = 50.f;

	/**
	 * How many navmesh points to sample per retreat decision.
	 *
	 * Low because this re-runs whenever a leg finishes: a handful of samples that
	 * keeps re-deciding beats one exhaustive search that commits the NPC to a
	 * point the player has since walked past.
	 */
	UPROPERTY(EditAnywhere, Category = "Movement", meta = (ClampMin = "1", ClampMax = "32"))
	int32 SampleCount = 8;

	/**
	 * Shortest gap between retreat decisions.
	 *
	 * A FLOOR ON RE-DECIDING, not a tick rate. MoveToLocation reports AlreadyAtGoal
	 * when the sampled point is where the pawn is standing, which leaves path
	 * following idle -- so without this the task re-samples the navmesh every
	 * frame for as long as the retreat is blocked, which is precisely the case
	 * where it is blocked and re-sampling is least likely to help.
	 */
	UPROPERTY(EditAnywhere, Category = "Movement", meta = (ClampMin = "0.0"))
	float RedecideInterval = 0.5f;

	/** Optional EQS query to pick the retreat point. See the class comment. */
	UPROPERTY(EditAnywhere, Category = "Movement")
	TObjectPtr<UEnvQuery> RetreatQuery = nullptr;

	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context,
		const FStateTreeTransitionResult& Transition) const override;
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context,
		const float DeltaTime) const override;
	virtual void ExitState(FStateTreeExecutionContext& Context,
		const FStateTreeTransitionResult& Transition) const override;
};

USTRUCT()
struct ARPGAI_API FARPGStateTreeUseConsumableInstanceData
{
	GENERATED_BODY()
};

/** Drinks from the quick-slot bar. The other half of "flees and heals". */
USTRUCT(meta = (DisplayName = "ARPG Use Consumable", Category = "ARPG|Combat"))
struct ARPGAI_API FARPGStateTreeTask_UseConsumable : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FARPGStateTreeUseConsumableInstanceData;

	virtual const UStruct* GetInstanceDataType() const override
	{
		return FInstanceDataType::StaticStruct();
	}

	/**
	 * Which slot to use. -1 uses whatever is selected.
	 *
	 * An NPC has no thumbs, so unlike the player it addresses a slot directly
	 * rather than cycling to it.
	 */
	UPROPERTY(EditAnywhere, Category = "Consumable")
	int32 SlotIndex = -1;

	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context,
		const FStateTreeTransitionResult& Transition) const override;
};
