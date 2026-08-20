// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AAIController;
class APawn;
struct FStateTreeExecutionContext;

/**
 * Reaching the pawn and controller from inside a StateTree node.
 *
 * WHY A HELPER RATHER THAN THE SCHEMA'S CONTEXT OBJECT. A StateTree can be run
 * by UStateTreeAIComponent (owner: the AI controller) or by a plain
 * UStateTreeComponent (owner: the actor), and the schema decides which context
 * objects exist. Resolving defensively here means one node works under either,
 * which is what lets a tree be reused on something that is not an AI-controlled
 * pawn later without every node needing a second code path.
 *
 * This is the same shape UARPGPerceptionComponent::GetBlackboard used to have
 * for exactly the same reason -- the component can live on either end.
 */
namespace ARPGStateTree
{
	/** The AI controller driving this tree, whichever end owns the component. */
	ARPGAI_API AAIController* GetController(const FStateTreeExecutionContext& Context);

	/** The pawn being driven. Null when nobody is possessing anything. */
	ARPGAI_API APawn* GetPawn(const FStateTreeExecutionContext& Context);
}
