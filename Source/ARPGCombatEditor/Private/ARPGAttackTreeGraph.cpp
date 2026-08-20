// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGAttackTreeGraph.h"
#include "ARPGAttackTreeGraphNode.h"
#include "ARPGCombatEditorModule.h"
#include "ARPGWeaponAttackTree.h"
#include "EdGraph/EdGraphPin.h"

namespace
{
	/** Column and row spacing for a graph laid out from the asset. */
	constexpr int32 ColumnWidth = 360;
	constexpr int32 RowHeight = 190;
}

UARPGWeaponAttackTree* UARPGAttackTreeGraph::GetAttackTree() const
{
	return Cast<UARPGWeaponAttackTree>(GetOuter());
}

void UARPGAttackTreeGraph::NotifyGraphChanged()
{
	Super::NotifyGraphChanged();
	CompileToAsset();
}

void UARPGAttackTreeGraph::UpdateAsset(int32 UpdateFlags)
{
	CompileToAsset();
}

// ---------------------------------------------------------------------------
// Graph -> asset
// ---------------------------------------------------------------------------

void UARPGAttackTreeGraph::CompileToAsset()
{
	if (bRebuilding)
	{
		return;
	}

	UARPGWeaponAttackTree* Tree = GetAttackTree();
	if (!Tree)
	{
		return;
	}

	Tree->Modify();

	// `Nodes` unqualified is UEdGraph::Nodes -- the boxes on the canvas. The
	// asset's own node array is Tree->Nodes, and the two are different lists of
	// different types; the local below is named for the one that ships.
	TArray<TObjectPtr<UARPGComboAttackNode>> RuntimeNodes;

	// Pass one: every box gets a runtime node, and every follow-up is cleared.
	// CLEARED FIRST, as a whole pass, because a wire that was deleted leaves no
	// trace to find in pass two -- the only way to know a follow-up is gone is
	// to start from none and re-derive all of them from what is on the canvas.
	for (UEdGraphNode* GraphNode : Nodes)
	{
		UARPGAttackTreeGraphNode_Attack* AttackNode = Cast<UARPGAttackTreeGraphNode_Attack>(GraphNode);
		if (!AttackNode)
		{
			continue;
		}

		AttackNode->EnsureComboNode();
		if (UARPGComboAttackNode* Combo = AttackNode->ComboNode)
		{
			Combo->Modify();
			for (uint8 Index = 0; Index < static_cast<uint8>(EARPGAttackInput::MAX); ++Index)
			{
				Combo->SetFollow(static_cast<EARPGAttackInput>(Index), nullptr);
			}
			RuntimeNodes.Add(Combo);
		}
	}

	for (uint8 Index = 0; Index < static_cast<uint8>(EARPGAttackInput::MAX); ++Index)
	{
		const EARPGAttackInput Input = static_cast<EARPGAttackInput>(Index);
		Tree->SetRoot(Input, nullptr);
		Tree->SetParryFollowup(Input, nullptr);
	}

	// Pass two: the wires.
	for (UEdGraphNode* GraphNode : Nodes)
	{
		UARPGAttackTreeGraphNodeBase* Node = Cast<UARPGAttackTreeGraphNodeBase>(GraphNode);
		if (!Node)
		{
			continue;
		}

		const bool bIsEntry = Node->IsA<UARPGAttackTreeGraphNode_Entry>();
		UARPGComboAttackNode* SourceCombo = bIsEntry
			? nullptr
			: Cast<UARPGAttackTreeGraphNode_Attack>(Node)->ComboNode;

		if (!bIsEntry && !SourceCombo)
		{
			continue;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output || Pin->LinkedTo.IsEmpty())
			{
				continue;
			}

			EARPGAttackInput Input = EARPGAttackInput::Light;
			bool bParry = false;
			if (!ARPGAttackTreeGraph::ParsePinName(Pin->PinName, Input, bParry))
			{
				// A pin whose name means nothing to the asset. Refused rather
				// than guessed at: silently dropping it would lose a
				// follow-up the author can see on screen.
				UE_LOG(LogARPGCombatEditor, Warning,
					TEXT("Attack tree '%s': unrecognised pin '%s'; its wire is not compiled."),
					*Tree->GetName(), *Pin->PinName.ToString());
				continue;
			}

			// Output pins are single-link by schema, so the first is the only.
			const UEdGraphPin* Target = Pin->LinkedTo[0];
			const UARPGAttackTreeGraphNode_Attack* TargetNode = Target
				? Cast<UARPGAttackTreeGraphNode_Attack>(Target->GetOwningNode())
				: nullptr;

			UARPGComboAttackNode* TargetCombo = TargetNode ? TargetNode->ComboNode : nullptr;
			if (!TargetCombo)
			{
				continue;
			}

			if (bIsEntry)
			{
				bParry ? Tree->SetParryFollowup(Input, TargetCombo)
				       : Tree->SetRoot(Input, TargetCombo);
			}
			else
			{
				SourceCombo->SetFollow(Input, TargetCombo);
			}
		}
	}

	// Whole-list assignment, so a node deleted from the canvas leaves the asset
	// on the same edit rather than lingering until something else rebuilds.
	Tree->Nodes = MoveTemp(RuntimeNodes);
	Tree->MarkPackageDirty();
}

// ---------------------------------------------------------------------------
// Asset -> graph
// ---------------------------------------------------------------------------

void UARPGAttackTreeGraph::RebuildFromAsset()
{
	UARPGWeaponAttackTree* Tree = GetAttackTree();
	if (!Tree)
	{
		return;
	}

	// Every step below fires NotifyGraphChanged. Without this the first placed
	// node would compile a one-node asset over the tree still being read.
	TGuardValue<bool> Guard(bRebuilding, true);

	// A script-built or pre-graph asset has references but no registry.
	if (Tree->Nodes.IsEmpty())
	{
		Tree->RebuildNodeRegistry();
	}

	while (Nodes.Num() > 0)
	{
		RemoveNode(Nodes[0]);
	}

	FGraphNodeCreator<UARPGAttackTreeGraphNode_Entry> EntryCreator(*this);
	UARPGAttackTreeGraphNode_Entry* Entry = EntryCreator.CreateNode(/*bSelectNewNode=*/false);
	EntryCreator.Finalize();

	TMap<UARPGComboAttackNode*, UARPGAttackTreeGraphNode_Attack*> Boxes;
	for (const TObjectPtr<UARPGComboAttackNode>& Combo : Tree->Nodes)
	{
		if (!Combo || Boxes.Contains(Combo))
		{
			continue;
		}

		// A node from a pre-graph asset is outered to its PARENT NODE, because
		// that is what an Instanced property meant: the child lived inside the
		// thing that referenced it. Nothing breaks if it stays there -- the
		// reference keeps the whole chain alive -- but deleting a beat would
		// then leave its follow-ups nested inside an object nothing draws.
		// Re-homing them onto the asset makes the flat array true of the
		// objects and not only of the pointers.
		if (Combo->GetOuter() != Tree)
		{
			Combo->Rename(nullptr, Tree, REN_DontCreateRedirectors);
		}

		FGraphNodeCreator<UARPGAttackTreeGraphNode_Attack> Creator(*this);
		UARPGAttackTreeGraphNode_Attack* Box = Creator.CreateNode(/*bSelectNewNode=*/false);
		Box->ComboNode = Combo;
		Creator.Finalize();

		Boxes.Add(Combo, Box);
	}

	// --- Wires ---------------------------------------------------------------

	auto Connect = [&Boxes](UEdGraphPin* From, UARPGComboAttackNode* To)
	{
		if (!From || !To)
		{
			return;
		}
		if (UARPGAttackTreeGraphNode_Attack** Box = Boxes.Find(To))
		{
			if (UEdGraphPin* In = (*Box)->FindPin(ARPGAttackTreeGraph::InputPinName, EGPD_Input))
			{
				From->MakeLinkTo(In);
			}
		}
	};

	for (uint8 Index = 0; Index < static_cast<uint8>(EARPGAttackInput::MAX); ++Index)
	{
		const EARPGAttackInput Input = static_cast<EARPGAttackInput>(Index);
		Connect(Entry->FindOutputPin(Input, /*bParry=*/false), Tree->GetRoot(Input));
		Connect(Entry->FindOutputPin(Input, /*bParry=*/true), Tree->GetParryFollowup(Input));
	}

	for (const TPair<UARPGComboAttackNode*, UARPGAttackTreeGraphNode_Attack*>& Pair : Boxes)
	{
		for (uint8 Index = 0; Index < static_cast<uint8>(EARPGAttackInput::MAX); ++Index)
		{
			const EARPGAttackInput Input = static_cast<EARPGAttackInput>(Index);
			Connect(Pair.Value->FindOutputPin(Input), Pair.Key->GetFollow(Input));
		}
	}

	// --- Layout --------------------------------------------------------------
	//
	// Breadth-first from the entry, one column per beat, so a chain reads left
	// to right the way it is played. Positions are not saved anywhere in the
	// asset -- they exist in the graph only, so this runs once and whatever the
	// author drags afterwards is what persists.

	TMap<UARPGComboAttackNode*, int32> Depth;
	TArray<UARPGComboAttackNode*> Frontier;

	for (uint8 Index = 0; Index < static_cast<uint8>(EARPGAttackInput::MAX); ++Index)
	{
		const EARPGAttackInput Input = static_cast<EARPGAttackInput>(Index);
		for (UARPGComboAttackNode* Start : { Tree->GetRoot(Input), Tree->GetParryFollowup(Input) })
		{
			if (Start && !Depth.Contains(Start))
			{
				Depth.Add(Start, 0);
				Frontier.Add(Start);
			}
		}
	}

	// The visited map doubles as the cycle guard: a moveset that loops back to
	// its opener is legal, and a queue that did not check would never drain.
	for (int32 Cursor = 0; Cursor < Frontier.Num(); ++Cursor)
	{
		UARPGComboAttackNode* Current = Frontier[Cursor];
		const int32 NextDepth = Depth[Current] + 1;

		for (uint8 Index = 0; Index < static_cast<uint8>(EARPGAttackInput::MAX); ++Index)
		{
			UARPGComboAttackNode* Follow = Current->GetFollow(static_cast<EARPGAttackInput>(Index));
			if (Follow && !Depth.Contains(Follow))
			{
				Depth.Add(Follow, NextDepth);
				Frontier.Add(Follow);
			}
		}
	}

	int32 MaxDepth = 0;
	for (const TPair<UARPGComboAttackNode*, int32>& Pair : Depth)
	{
		MaxDepth = FMath::Max(MaxDepth, Pair.Value);
	}

	TMap<int32, int32> RowsUsed;
	auto Place = [&RowsUsed](UEdGraphNode* Node, int32 Column)
	{
		int32& Row = RowsUsed.FindOrAdd(Column);
		Node->NodePosX = Column * ColumnWidth;
		Node->NodePosY = Row * RowHeight;
		++Row;
	};

	Place(Entry, 0);

	for (const TPair<UARPGComboAttackNode*, UARPGAttackTreeGraphNode_Attack*>& Pair : Boxes)
	{
		// An unreachable node -- placed but never wired up -- goes in a column
		// past the end rather than on top of the entry, where it would look
		// like a root.
		const int32* Found = Depth.Find(Pair.Key);
		Place(Pair.Value, Found ? *Found + 1 : MaxDepth + 2);
	}
}
