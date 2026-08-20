// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "ARPGTestFixtures.h"

#if WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS

#include "ARPGNPCDefinition.h"
#include "StateTree.h"

/**
 * The test NPC archetype actually runs a compiled behaviour tree.
 *
 * WHY THIS EXISTS. DA_NPC_TestDummy shipped with StateTreeRef unset and no
 * StateTree asset anywhere in the project. Everything else about the NPC was
 * correct -- body, archetype, weapon, faction, mesh -- so it spawned, perceived,
 * and stood perfectly still. Nothing warned: UARPGNPCComponent treats an unset
 * tree as "this NPC is not driven", which is a legitimate state for a prop or a
 * corpse and therefore not something it can complain about.
 *
 * TWO ASSERTIONS, AND THE SECOND IS THE ONE THAT MATTERS. That a tree is
 * assigned is easy; that it is COMPILED is the part that silently fails. A
 * StateTree's runtime reads a baked representation rather than the editor data,
 * so an authored-but-uncompiled tree loads, resolves, reports no error, and runs
 * nothing at all. IsReadyToRun() is the difference.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGNPCHasCompiledTreeTest,
	"ARPG.AI.NPC.ArchetypeRunsACompiledTree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGNPCHasCompiledTreeTest::RunTest(const FString& Parameters)
{
	UARPGNPCDefinition* Definition = LoadObject<UARPGNPCDefinition>(
		nullptr, TEXT("/Game/ARPG/NPCs/DA_NPC_TestDummy.DA_NPC_TestDummy"));

	if (!TestNotNull(TEXT("DA_NPC_TestDummy loads"), Definition))
	{
		return false;
	}

	const UStateTree* Tree = Definition->StateTreeRef.GetStateTree();
	if (!TestNotNull(TEXT("The archetype names a StateTree"), Tree))
	{
		AddError(TEXT("StateTreeRef is unset, so this NPC will spawn and stand still. ")
			TEXT("Run: UnrealEditor-Cmd <project> -run=ARPGAIEditor.ARPGGenerateNPCTree"));
		return false;
	}

	// THE ASSERTION THAT EARNS ITS KEEP. A tree that was authored but never
	// compiled passes every check above this line and still does nothing.
	TestTrue(TEXT("...and that tree is compiled and ready to run"), Tree->IsReadyToRun());

	// A compiled tree with no states is the other way to be inert: it runs, and
	// immediately has nowhere to go.
	TestTrue(TEXT("...and it has states to select"), Tree->GetStates().Num() > 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS
