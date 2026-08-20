// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "ARPGGenerateNPCTreeCommandlet.generated.h"

/**
 * Authors and compiles the placeholder NPC StateTree, then points the test
 * archetype at it.
 *
 *   UnrealEditor-Cmd.exe <project>.uproject -run=ARPGGenerateNPCTree
 *
 * WHY A COMMANDLET AND NOT A PYTHON SCRIPT, given every other generator in
 * Tools/ is Python. Three things a StateTree needs are unreachable from the
 * Python bridge on this engine build, and Tools/generate_npc_statetree.py exists
 * only to say so:
 *
 *   - UStateTreeFactory::StateTreeSchemaClass is PROTECTED, so the schema cannot
 *     be chosen from script; without it the factory falls back to a modal class
 *     picker, which returns null under -unattended.
 *   - UStateTreeEditingSubsystem::CompileStateTree is a plain static, not a
 *     UFUNCTION. An uncompiled tree is INERT -- the runtime reads a baked
 *     representation, not the editor data -- so a script that populates states
 *     and cannot compile has produced an asset that loads and does nothing.
 *   - FStateTreePropertyPathBinding is not exposed at all.
 *
 * All three are ordinary C++ from in here.
 *
 * WHAT IT WRITES, and what it deliberately does not. The tree is a PLACEHOLDER
 * and only uses nodes that need no property bindings:
 *
 *     Root
 *       Recover  [ARPG Health Below 0.35]  -> ARPG Use Consumable
 *       Fight                              -> ARPG Attack (Light)
 *
 * That is an NPC that swings, and drinks when it is hurt. It is observable in a
 * running game, which is the whole point of a placeholder, and every node in it
 * reads its pawn from the execution context rather than from a bound input.
 *
 * THE COMBAT STATES ARE NOT HERE ON PURPOSE. Perception, Attempt Parry, Face
 * Target, Create Space and the range conditions all take a Target, which has to
 * be BOUND to the perception evaluator's output. A binding is a
 * FStateTreePropertyPathBinding carrying the source node's GUID and a property
 * path, and getting one subtly wrong produces a tree that compiles clean and
 * silently runs with a null target -- the exact failure this commandlet exists
 * to avoid manufacturing. The full tree is ten minutes of dragging in the
 * StateTree editor and the recipe is in Docs/TEST_NPC_SETUP.md; this gives that
 * work a compiled, wired asset to extend instead of a blank one.
 *
 * RE-RUNNABLE. An existing tree is rebuilt from scratch rather than appended to,
 * so running it twice does not produce two Fight states.
 */
UCLASS()
class UARPGGenerateNPCTreeCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UARPGGenerateNPCTreeCommandlet();

	virtual int32 Main(const FString& Params) override;
};
