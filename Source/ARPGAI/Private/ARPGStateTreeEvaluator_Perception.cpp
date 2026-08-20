// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGStateTreeEvaluator_Perception.h"
#include "ARPGPerceptionComponent.h"
#include "ARPGStateTreeContext.h"
#include "AIController.h"
#include "GameFramework/Pawn.h"

namespace
{
	UARPGPerceptionComponent* FindPerception(const FStateTreeExecutionContext& Context)
	{
		// On the CONTROLLER, not the pawn -- perception is knowledge, and knowledge
		// belongs to the mind. See AARPGAIController.
		const AAIController* Controller = ARPGStateTree::GetController(Context);
		return Controller ? Controller->FindComponentByClass<UARPGPerceptionComponent>() : nullptr;
	}
}

void FARPGStateTreeEvaluator_Perception::TreeStart(FStateTreeExecutionContext& Context) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);

	// Home is captured once, here, rather than authored: "home" for a placed
	// enemy is simply where the designer put it, and re-reading it every tick
	// would make it follow the NPC and leash nothing.
	if (const APawn* Pawn = ARPGStateTree::GetPawn(Context))
	{
		Data.HomeLocation = Pawn->GetActorLocation();
	}
}

void FARPGStateTreeEvaluator_Perception::Tick(FStateTreeExecutionContext& Context,
	const float DeltaTime) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);

	const UARPGPerceptionComponent* Perception = FindPerception(Context);
	if (!Perception)
	{
		// An unpossessed pawn, or one whose controller carries no senses. Reported
		// as "knows nothing" rather than left stale, so a tree cannot keep acting
		// on a target that nothing is perceiving any more.
		Data.Target = nullptr;
		Data.bHasTarget = false;
		Data.bAlerted = false;
		return;
	}

	Data.Target = Perception->GetTarget();
	Data.bHasTarget = Data.Target != nullptr;
	Data.bAlerted = Perception->IsAlerted();

	Data.LastKnownLocation = Perception->GetLastKnownLocation();

	Data.bHasInvestigateLocation = Perception->HasInvestigateLocation();
	if (Data.bHasInvestigateLocation)
	{
		Data.InvestigateLocation = Perception->GetInvestigateLocation();
	}
}
