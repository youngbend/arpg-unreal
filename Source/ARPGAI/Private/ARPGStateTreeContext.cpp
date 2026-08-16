// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGStateTreeContext.h"
#include "AIController.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Pawn.h"
#include "StateTreeExecutionContext.h"

namespace ARPGStateTree
{
	AAIController* GetController(const FStateTreeExecutionContext& Context)
	{
		UObject* Owner = Context.GetOwner();
		if (!Owner)
		{
			return nullptr;
		}

		// Owned by the controller: UStateTreeAIComponent's case, and the common one.
		if (AAIController* Controller = Cast<AAIController>(Owner))
		{
			return Controller;
		}

		// Owned by a component on either end -- unwrap once and re-test.
		if (const UActorComponent* Component = Cast<UActorComponent>(Owner))
		{
			Owner = Component->GetOwner();
		}

		if (AAIController* Controller = Cast<AAIController>(Owner))
		{
			return Controller;
		}

		if (const APawn* Pawn = Cast<APawn>(Owner))
		{
			return Cast<AAIController>(Pawn->GetController());
		}

		return nullptr;
	}

	APawn* GetPawn(const FStateTreeExecutionContext& Context)
	{
		if (const AAIController* Controller = GetController(Context))
		{
			return Controller->GetPawn();
		}

		// No controller does not have to mean no pawn: a tree run directly on an
		// unpossessed pawn still has a body to act on, and the tasks that only
		// touch components (attack, consumable) work perfectly well that way.
		UObject* Owner = Context.GetOwner();
		if (const UActorComponent* Component = Cast<UActorComponent>(Owner))
		{
			Owner = Component->GetOwner();
		}

		return Cast<APawn>(Owner);
	}
}
