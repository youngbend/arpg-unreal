// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "ARPGWeaponAttackTree.h"
#include "ARPGBTTasks.generated.h"

/**
 * The domain actions, as behaviour-tree tasks.
 *
 * Movement, waiting and blackboard manipulation are already engine tasks and are
 * deliberately not ported -- what remains is only the part that drives THIS
 * game's combat components.
 */

/**
 * Throws one attack through the combo component.
 *
 * Returns immediately rather than waiting for the swing: the combo component
 * owns the beat, and a task that blocked until the montage finished would stop
 * the tree re-evaluating its higher-priority branches -- which is exactly when a
 * reactive parry or a flee needs to interrupt.
 */
UCLASS()
class ARPGAI_API UARPGBTTask_Attack : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UARPGBTTask_Attack();

	/** Which input this beat represents, so a tree can author heavy openers. */
	UPROPERTY(EditAnywhere, Category = "Attack")
	EARPGAttackInput Input = EARPGAttackInput::Light;

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp,
		uint8* NodeMemory) override;

	virtual FString GetStaticDescription() const override;
};

/**
 * Turns to face the target, finishing once within tolerance.
 *
 * A RUNNING task rather than an instant snap, because the turn is the tell: an
 * NPC that pivots instantly to face you removes the window a player reads to
 * decide whether to close or back off.
 */
UCLASS()
class ARPGAI_API UARPGBTTask_FaceTarget : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UARPGBTTask_FaceTarget();

	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector TargetKey;

	UPROPERTY(EditAnywhere, Category = "Facing", meta = (ClampMin = "0.0"))
	float TurnSpeedDegrees = 360.f;

	UPROPERTY(EditAnywhere, Category = "Facing",
		meta = (ClampMin = "0.0", ClampMax = "180.0"))
	float ToleranceDegrees = 5.f;

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp,
		uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory,
		float DeltaSeconds) override;

	virtual void InitializeFromAsset(UBehaviorTree& Asset) override;
	virtual FString GetStaticDescription() const override;
};

/**
 * Moves directly AWAY from the target until far enough, for backing off or
 * fleeing.
 *
 * Distinct from a move-to because there is no destination: the goal is a
 * distance, and a fixed retreat point would walk the NPC into a corner and hold
 * it there while the player closed in.
 */
UCLASS()
class ARPGAI_API UARPGBTTask_CreateSpace : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UARPGBTTask_CreateSpace();

	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector TargetKey;

	UPROPERTY(EditAnywhere, Category = "Movement", meta = (ClampMin = "0.0"))
	float DesiredDistance = 600.f;

	/** Gives up rather than backing away forever into terrain. */
	UPROPERTY(EditAnywhere, Category = "Movement", meta = (ClampMin = "0.1"))
	float Timeout = 4.f;

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp,
		uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory,
		float DeltaSeconds) override;

	virtual void InitializeFromAsset(UBehaviorTree& Asset) override;
	virtual uint16 GetInstanceMemorySize() const override { return sizeof(FMemory); }
	virtual FString GetStaticDescription() const override;

private:
	struct FMemory
	{
		float Elapsed = 0.f;
	};
};

/** Drinks from the quick-slot bar. The other half of "flees and heals". */
UCLASS()
class ARPGAI_API UARPGBTTask_UseConsumable : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UARPGBTTask_UseConsumable();

	/**
	 * Which slot to use. -1 uses whatever is selected.
	 *
	 * An NPC has no thumbs, so unlike the player it addresses a slot directly
	 * rather than cycling to it.
	 */
	UPROPERTY(EditAnywhere, Category = "Consumable")
	int32 SlotIndex = -1;

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp,
		uint8* NodeMemory) override;

	virtual FString GetStaticDescription() const override;
};
