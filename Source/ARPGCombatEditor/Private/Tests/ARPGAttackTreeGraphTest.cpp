// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ARPGAttackDefinition.h"
#include "ARPGAttackTreeGraph.h"
#include "ARPGAttackTreeGraphNode.h"
#include "ARPGAttackTreeGraphSchema.h"
#include "ARPGWeaponAttackTree.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/BlueprintEditorUtils.h"

/**
 * The graph editor's compile step, without a window.
 *
 * WORTH TESTING HEADLESSLY because the compile is where authoring becomes game
 * data: everything above it is Slate, which a test cannot meaningfully drive,
 * and everything below it is the combo component, which is already covered. The
 * translation between them is the part that can silently write the wrong
 * moveset, and it is pure object graph manipulation with no UI in it at all.
 */
namespace ARPGAttackTreeGraphTestUtils
{
	UARPGWeaponAttackTree* MakeTree()
	{
		UARPGWeaponAttackTree* Tree = NewObject<UARPGWeaponAttackTree>(GetTransientPackage());

		Tree->EdGraph = FBlueprintEditorUtils::CreateNewGraph(Tree, TEXT("AttackTree"),
			UARPGAttackTreeGraph::StaticClass(), UARPGAttackTreeGraphSchema::StaticClass());

		return Tree;
	}

	UARPGAttackTreeGraph* GetGraph(UARPGWeaponAttackTree* Tree)
	{
		return CastChecked<UARPGAttackTreeGraph>(Tree->EdGraph);
	}

	UARPGAttackTreeGraphNode_Entry* FindEntry(UEdGraph* Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UARPGAttackTreeGraphNode_Entry* Entry = Cast<UARPGAttackTreeGraphNode_Entry>(Node))
			{
				return Entry;
			}
		}
		return nullptr;
	}

	UARPGAttackTreeGraphNode_Attack* AddBeat(UEdGraph* Graph, const TCHAR* AttackId)
	{
		FGraphNodeCreator<UARPGAttackTreeGraphNode_Attack> Creator(*Graph);
		UARPGAttackTreeGraphNode_Attack* Node = Creator.CreateNode(/*bSelectNewNode=*/false);
		Creator.Finalize();

		// PostPlacedNewNode made the runtime node; this only fills it in.
		Node->EnsureComboNode();
		if (Node->ComboNode)
		{
			UARPGAttackDefinition* Attack = NewObject<UARPGAttackDefinition>(Node->ComboNode);
			Attack->AttackId = AttackId;
			Node->ComboNode->Attack = Attack;
		}

		return Node;
	}

	void Wire(UARPGAttackTreeGraphNodeBase* From, EARPGAttackInput Input,
		UARPGAttackTreeGraphNode_Attack* To, bool bParry = false)
	{
		UEdGraphPin* Out = From->FindOutputPin(Input, bParry);
		UEdGraphPin* In = To->FindPin(ARPGAttackTreeGraph::InputPinName, EGPD_Input);
		if (Out && In)
		{
			Out->MakeLinkTo(In);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGAttackTreeGraphCompileTest,
	"ARPG.Combat.AttackTreeGraph.CompilesToAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGAttackTreeGraphCompileTest::RunTest(const FString& Parameters)
{
	using namespace ARPGAttackTreeGraphTestUtils;

	UARPGWeaponAttackTree* Tree = MakeTree();
	UARPGAttackTreeGraph* Graph = GetGraph(Tree);

	Graph->RebuildFromAsset(); // an empty asset: entry node only
	UARPGAttackTreeGraphNode_Entry* Entry = FindEntry(Graph);
	if (!Entry)
	{
		AddError(TEXT("A rebuilt graph has no entry node; nothing else can be asserted."));
		return false;
	}

	UARPGAttackTreeGraphNode_Attack* Opener = AddBeat(Graph, TEXT("opener"));
	UARPGAttackTreeGraphNode_Attack* Finisher = AddBeat(Graph, TEXT("finisher"));

	Wire(Entry, EARPGAttackInput::Light, Opener);
	Wire(Opener, EARPGAttackInput::Light, Finisher);

	// The same finisher off two different buttons -- one node, two parents. The
	// arrangement the old nested-subobject asset could not hold, and faked by
	// copying the node.
	Wire(Opener, EARPGAttackInput::Heavy, Finisher);
	Wire(Entry, EARPGAttackInput::Light, Finisher, /*bParry=*/true);

	Graph->CompileToAsset();

	TestSamePtr(TEXT("Light root is the wired opener"),
		Tree->GetRoot(EARPGAttackInput::Light), Opener->ComboNode.Get());
	TestNull(TEXT("An unwired root stays empty"), Tree->GetRoot(EARPGAttackInput::Heavy));

	TestSamePtr(TEXT("The opener's light follow-up is the finisher"),
		Opener->ComboNode->GetFollow(EARPGAttackInput::Light), Finisher->ComboNode.Get());
	TestSamePtr(TEXT("Its heavy follow-up is the SAME node, not a copy"),
		Opener->ComboNode->GetFollow(EARPGAttackInput::Heavy), Finisher->ComboNode.Get());
	TestSamePtr(TEXT("...as is the parry follow-up"),
		Tree->GetParryFollowup(EARPGAttackInput::Light), Finisher->ComboNode.Get());

	TestTrue(TEXT("The finisher is a leaf"), Finisher->ComboNode->IsLeaf());
	TestEqual(TEXT("Both beats are registered, each once"), Tree->Nodes.Num(), 2);

	// A wire removed has to leave the asset on the same edit -- the compile
	// clears every follow-up before re-deriving them, and this is what says so.
	UEdGraphPin* HeavyPin = Opener->FindOutputPin(EARPGAttackInput::Heavy);
	HeavyPin->BreakAllPinLinks();
	Graph->CompileToAsset();

	TestNull(TEXT("Breaking the heavy wire clears the follow-up"),
		Opener->ComboNode->GetFollow(EARPGAttackInput::Heavy));
	TestSamePtr(TEXT("...and leaves the light one alone"),
		Opener->ComboNode->GetFollow(EARPGAttackInput::Light), Finisher->ComboNode.Get());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGAttackTreeGraphRebuildTest,
	"ARPG.Combat.AttackTreeGraph.RebuildsFromScriptAuthoredAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * The case every tree in the project is in today: arrays authored by
 * Tools/build_sword_attack_tree.py and no graph at all. Opening one has to draw
 * what is there -- if it drew nothing, the first save would compile that nothing
 * straight over the moveset.
 */
bool FARPGAttackTreeGraphRebuildTest::RunTest(const FString& Parameters)
{
	using namespace ARPGAttackTreeGraphTestUtils;

	UARPGWeaponAttackTree* Tree = MakeTree();

	// Built the way a script builds one: nodes wired to each other, no registry
	// and no graph.
	UARPGComboAttackNode* Opener = NewObject<UARPGComboAttackNode>(Tree);
	UARPGComboAttackNode* Finisher = NewObject<UARPGComboAttackNode>(Tree);
	Opener->FollowLight = Finisher;
	Opener->FollowHeavy = Finisher; // shared, as the sword tree's finishers are
	Tree->RootLight = Opener;
	Tree->ParryLight = Finisher;

	TestEqual(TEXT("Setup: a script-built tree has no registry"), Tree->Nodes.Num(), 0);

	UARPGAttackTreeGraph* Graph = GetGraph(Tree);
	Graph->RebuildFromAsset();

	TestEqual(TEXT("Both runtime nodes are registered by the rebuild"), Tree->Nodes.Num(), 2);
	TestEqual(TEXT("The canvas has an entry node and one box per runtime node"),
		Graph->Nodes.Num(), 3);

	// A round trip has to be a no-op. Anything the rebuild fails to draw, the
	// compile immediately deletes -- so this is the assertion that matters.
	Graph->CompileToAsset();

	TestSamePtr(TEXT("Round trip keeps the light root"), Tree->GetRoot(EARPGAttackInput::Light),
		Opener);
	TestSamePtr(TEXT("Round trip keeps the light follow-up"),
		Opener->GetFollow(EARPGAttackInput::Light), Finisher);
	TestSamePtr(TEXT("Round trip keeps the shared heavy follow-up"),
		Opener->GetFollow(EARPGAttackInput::Heavy), Finisher);
	TestSamePtr(TEXT("Round trip keeps the parry follow-up"),
		Tree->GetParryFollowup(EARPGAttackInput::Light), Finisher);
	TestEqual(TEXT("Round trip keeps both nodes and invents none"), Tree->Nodes.Num(), 2);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
