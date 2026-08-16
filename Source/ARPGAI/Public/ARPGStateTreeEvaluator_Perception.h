// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeExecutionContext.h"
#include "ARPGStateTreeEvaluator_Perception.generated.h"

class AActor;

/**
 * What this NPC currently knows, as bindable StateTree properties.
 *
 * THIS IS THE BLACKBOARD'S REPLACEMENT. Under behaviour trees the perception
 * component wrote five named keys into a UBlackboardComponent and every node
 * looked them up by FName -- a contract between asset and C++ that nothing
 * enforced, where a typo on either side produced a node that silently did
 * nothing rather than failing. ARPGBlackboardKeys.h existed purely to centralise
 * those strings, and centralising a string is not the same as checking it.
 *
 * StateTree binds properties instead of naming keys, so the editor resolves
 * these against the consuming node at author time and the compiler resolves the
 * struct. The whole class of typo bug is gone rather than mitigated.
 *
 * AN EVALUATOR RATHER THAN PER-NODE READS, because every node wanting the target
 * would otherwise re-find the perception component itself, and because a bound
 * property is what the editor can offer as a dropdown when a designer wires a
 * transition. Read once per tick, at the top of the tree.
 */
USTRUCT()
struct ARPGAI_API FARPGStateTreePerceptionInstanceData
{
	GENERATED_BODY()

	/** The actor this NPC is currently committed to fighting. */
	UPROPERTY(EditAnywhere, Category = "Output")
	TObjectPtr<AActor> Target = nullptr;

	/**
	 * Where the target was last actually perceived.
	 *
	 * Separate from the target itself because the two have different lifetimes:
	 * Target goes null the moment perception drops it, and this is what the NPC
	 * still has to go and look at.
	 */
	UPROPERTY(EditAnywhere, Category = "Output")
	FVector LastKnownLocation = FVector::ZeroVector;

	/** A place to walk to and look around, with no target granted. */
	UPROPERTY(EditAnywhere, Category = "Output")
	FVector InvestigateLocation = FVector::ZeroVector;

	/** Where the NPC started, for leash checks. Captured once, at tree start. */
	UPROPERTY(EditAnywhere, Category = "Output")
	FVector HomeLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "Output")
	bool bHasTarget = false;

	UPROPERTY(EditAnywhere, Category = "Output")
	bool bAlerted = false;

	UPROPERTY(EditAnywhere, Category = "Output")
	bool bHasInvestigateLocation = false;
};

USTRUCT(meta = (DisplayName = "ARPG Perception", Category = "ARPG|AI"))
struct ARPGAI_API FARPGStateTreeEvaluator_Perception : public FStateTreeEvaluatorCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FARPGStateTreePerceptionInstanceData;

	virtual const UStruct* GetInstanceDataType() const override
	{
		return FInstanceDataType::StaticStruct();
	}

	virtual void TreeStart(FStateTreeExecutionContext& Context) const override;
	virtual void Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
};
