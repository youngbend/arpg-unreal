// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGAttackTreeGraphSchema.h"
#include "ARPGAttackTreeGraph.h"
#include "ARPGAttackTreeGraphNode.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "ARPGAttackTreeGraphSchema"

UEdGraphNode* FARPGAttackTreeSchemaAction_NewNode::PerformAction(UEdGraph* ParentGraph,
	UEdGraphPin* FromPin, const FVector2f& Location, bool bSelectNewNode)
{
	if (!ParentGraph)
	{
		return nullptr;
	}

	const FScopedTransaction Transaction(LOCTEXT("AddBeatTransaction", "Add Combo Beat"));
	ParentGraph->Modify();
	if (FromPin)
	{
		FromPin->Modify();
	}

	FGraphNodeCreator<UARPGAttackTreeGraphNode_Attack> Creator(*ParentGraph);
	UARPGAttackTreeGraphNode_Attack* Node = Creator.CreateNode(bSelectNewNode);
	Node->NodePosX = static_cast<int32>(Location.X);
	Node->NodePosY = static_cast<int32>(Location.Y);
	Creator.Finalize();

	// Dragged off a follow-up pin: wire it up, which is the entire reason anyone
	// drags off a pin rather than right-clicking empty space.
	if (FromPin && FromPin->Direction == EGPD_Output)
	{
		if (UEdGraphPin* In = Node->FindPin(ARPGAttackTreeGraph::InputPinName, EGPD_Input))
		{
			ParentGraph->GetSchema()->TryCreateConnection(FromPin, In);
		}
	}

	ParentGraph->NotifyGraphChanged();
	return Node;
}

// ---------------------------------------------------------------------------

void UARPGAttackTreeGraphSchema::CreateDefaultNodesForGraph(UEdGraph& Graph) const
{
	FGraphNodeCreator<UARPGAttackTreeGraphNode_Entry> Creator(Graph);
	UARPGAttackTreeGraphNode_Entry* Entry = Creator.CreateNode(/*bSelectNewNode=*/false);
	Entry->NodePosX = 0;
	Entry->NodePosY = 0;
	Creator.Finalize();

	Graph.NotifyGraphChanged();
}

void UARPGAttackTreeGraphSchema::GetGraphContextActions(FGraphContextMenuBuilder& ContextMenuBuilder) const
{
	// ONE action, and no submenu of node types, because there is only one kind
	// of node to place: which attack a beat plays is a property of the node, set
	// in the details panel, not a choice of class made when placing it.
	TSharedPtr<FARPGAttackTreeSchemaAction_NewNode> Action =
		MakeShared<FARPGAttackTreeSchemaAction_NewNode>(
			FText::GetEmpty(),
			LOCTEXT("AddBeat", "Add combo beat"),
			LOCTEXT("AddBeatTooltip",
				"One beat of a combo. Assign its attack in the details panel, then wire "
				"its Light, Heavy and Special pins to whatever each button leads to."),
			0);

	ContextMenuBuilder.AddAction(Action);
}

const FPinConnectionResponse UARPGAttackTreeGraphSchema::CanCreateConnection(const UEdGraphPin* A,
	const UEdGraphPin* B) const
{
	if (!A || !B)
	{
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, LOCTEXT("NoPin", "Invalid pin."));
	}

	if (A->Direction == B->Direction)
	{
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW,
			LOCTEXT("SameDirection", "Connect a follow-up pin to a beat's input."));
	}

	// A beat may follow ITSELF -- that is a chain that repeats while the button
	// is held, which UARPGAttackDefinition::bContinuousHold exists for. The combo
	// component resolves one step per press, so nothing here can recurse.

	const UEdGraphPin* Out = (A->Direction == EGPD_Output) ? A : B;
	if (!Out->LinkedTo.IsEmpty())
	{
		// A button leads exactly one place from a given beat, so a second wire
		// REPLACES the first rather than being refused: refusing would have the
		// author break the old link by hand before making the obvious edit.
		return FPinConnectionResponse(
			(Out == A) ? CONNECT_RESPONSE_BREAK_OTHERS_A : CONNECT_RESPONSE_BREAK_OTHERS_B,
			LOCTEXT("ReplaceFollowup", "Replace the existing follow-up"));
	}

	return FPinConnectionResponse(CONNECT_RESPONSE_MAKE, LOCTEXT("Chain", "Chain into this beat"));
}

FLinearColor UARPGAttackTreeGraphSchema::GetPinTypeColor(const FEdGraphPinType& PinType) const
{
	return FLinearColor(0.55f, 0.75f, 1.f);
}

void UARPGAttackTreeGraphSchema::BreakPinLinks(UEdGraphPin& TargetPin, bool bSendsNodeNotifcation) const
{
	Super::BreakPinLinks(TargetPin, bSendsNodeNotifcation);

	if (const UEdGraphNode* Node = TargetPin.GetOwningNodeUnchecked())
	{
		if (UEdGraph* Graph = Node->GetGraph())
		{
			Graph->NotifyGraphChanged();
		}
	}
}

void UARPGAttackTreeGraphSchema::BreakNodeLinks(UEdGraphNode& TargetNode) const
{
	Super::BreakNodeLinks(TargetNode);

	if (UEdGraph* Graph = TargetNode.GetGraph())
	{
		Graph->NotifyGraphChanged();
	}
}

void UARPGAttackTreeGraphSchema::BreakSinglePinLink(UEdGraphPin* SourcePin, UEdGraphPin* TargetPin) const
{
	Super::BreakSinglePinLink(SourcePin, TargetPin);

	const UEdGraphNode* Node = SourcePin ? SourcePin->GetOwningNodeUnchecked() : nullptr;
	if (UEdGraph* Graph = Node ? Node->GetGraph() : nullptr)
	{
		Graph->NotifyGraphChanged();
	}
}

#undef LOCTEXT_NAMESPACE
