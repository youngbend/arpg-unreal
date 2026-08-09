// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "ARPGBTDecorators.generated.h"

/**
 * The domain conditions, as behaviour-tree decorators.
 *
 * Godot's tree had these as leaf NODES because it had no decorator concept worth
 * the name. UE has both, and a CONDITION is a decorator: it gates a branch
 * rather than doing anything. Expressing them correctly means they can abort
 * lower-priority branches when they change, which a leaf condition polled once
 * per tick cannot do -- an NPC whose target dies mid-swing gets out immediately
 * rather than at the end of whatever it was doing.
 *
 * Everything generic -- invert, cooldown, timeout, loop, blackboard-set -- is
 * already in the engine and is deliberately NOT ported.
 */

/** SUCCESS when the target is within the actor's own weapon reach. */
UCLASS()
class ARPGAI_API UARPGBTDecorator_TargetInAttackRange : public UBTDecorator
{
	GENERATED_BODY()

public:
	UARPGBTDecorator_TargetInAttackRange();

	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector TargetKey;

	/**
	 * Added to the weapon's own reach.
	 *
	 * Reach comes from the equipped weapon rather than being authored here, so a
	 * greatsword NPC engages from further out than a dagger one using the same
	 * tree -- which is the whole reason a tree is shared across archetypes.
	 */
	UPROPERTY(EditAnywhere, Category = "Range")
	float ReachTolerance = 50.f;

	virtual bool CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp,
		uint8* NodeMemory) const override;

	virtual void InitializeFromAsset(UBehaviorTree& Asset) override;
	virtual FString GetStaticDescription() const override;
};

/**
 * SUCCESS when the actor is already facing the target closely enough to commit.
 *
 * Paired with the range check to gate an attack, so the NPC lines up BEFORE
 * swinging rather than starting a swing while still turning. That matters most
 * with a slow, weighty attack rotation, where almost no reorientation happens
 * once the attack has begun.
 */
UCLASS()
class ARPGAI_API UARPGBTDecorator_FacingTarget : public UBTDecorator
{
	GENERATED_BODY()

public:
	UARPGBTDecorator_FacingTarget();

	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector TargetKey;

	UPROPERTY(EditAnywhere, Category = "Facing",
		meta = (ClampMin = "0.0", ClampMax = "180.0"))
	float MaxAngleDegrees = 20.f;

	virtual bool CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp,
		uint8* NodeMemory) const override;

	virtual void InitializeFromAsset(UBehaviorTree& Asset) override;
	virtual FString GetStaticDescription() const override;
};

/** SUCCESS when the actor's health fraction is below the threshold. */
UCLASS()
class ARPGAI_API UARPGBTDecorator_HealthBelow : public UBTDecorator
{
	GENERATED_BODY()

public:
	UARPGBTDecorator_HealthBelow();

	UPROPERTY(EditAnywhere, Category = "Health",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Threshold = 0.3f;

	virtual bool CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp,
		uint8* NodeMemory) const override;

	virtual FString GetStaticDescription() const override;
};

/**
 * SUCCESS when the actor has strayed further than LeashRange from home.
 *
 * ReleaseRange adds hysteresis: once leashed, the NPC stays leashed until it is
 * back inside the smaller radius. Without it an NPC hovering exactly at the
 * boundary flips between chasing and returning every tick, which reads as a
 * twitch rather than a decision.
 */
UCLASS()
class ARPGAI_API UARPGBTDecorator_IsLeashed : public UBTDecorator
{
	GENERATED_BODY()

public:
	UARPGBTDecorator_IsLeashed();

	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector HomeKey;

	UPROPERTY(EditAnywhere, Category = "Leash", meta = (ClampMin = "0.0"))
	float LeashRange = 2500.f;

	/** 0 disables hysteresis, releasing at exactly LeashRange. */
	UPROPERTY(EditAnywhere, Category = "Leash", meta = (ClampMin = "0.0"))
	float ReleaseRange = 0.f;

	virtual bool CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp,
		uint8* NodeMemory) const override;

	virtual void InitializeFromAsset(UBehaviorTree& Asset) override;
	virtual FString GetStaticDescription() const override;

	virtual uint16 GetInstanceMemorySize() const override { return sizeof(FMemory); }

private:
	/**
	 * Per-INSTANCE, because one tree asset is shared by every NPC using it.
	 *
	 * A latch stored on the decorator itself would be shared by every goblin in
	 * the level -- exactly the bug Godot's blackboard-keyed per-node state
	 * existed to avoid. UE's node memory is the same idea, provided by the
	 * engine.
	 */
	struct FMemory
	{
		bool bLeashed = false;
	};
};
