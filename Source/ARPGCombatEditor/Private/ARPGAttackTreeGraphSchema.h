// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphSchema.h"
#include "ARPGAttackTreeGraphSchema.generated.h"

/** Right-click (or drag off a pin) to place a beat. */
USTRUCT()
struct FARPGAttackTreeSchemaAction_NewNode : public FEdGraphSchemaAction
{
	GENERATED_USTRUCT_BODY()

	FARPGAttackTreeSchemaAction_NewNode() = default;

	FARPGAttackTreeSchemaAction_NewNode(FText InNodeCategory, FText InMenuDesc, FText InToolTip,
		int32 InGrouping)
		: FEdGraphSchemaAction(MoveTemp(InNodeCategory), MoveTemp(InMenuDesc), MoveTemp(InToolTip),
			InGrouping)
	{
	}

	// The Location parameter changed type in UE 5.6, when Slate moved to float
	// vectors: FVector2D before, FVector2f after. The FVector2D overload still
	// exists but is deprecated and no longer the one the menu calls, so the
	// SIGNATURE has to match the float one exactly -- otherwise this overrides
	// nothing and the menu item does nothing when clicked.
	virtual UEdGraphNode* PerformAction(UEdGraph* ParentGraph, UEdGraphPin* FromPin,
		const FVector2f& Location, bool bSelectNewNode = true) override;
};

/**
 * Connection rules for an attack tree.
 *
 * DERIVED FROM UEdGraphSchema RATHER THAN UAIGraphSchema, which is the one place
 * this editor steps outside AIGraph. That base exists to offer a menu of node
 * CLASSES -- behaviour tree tasks and decorators are picked by class, and it
 * carries a class cache to build that list. A beat here is not a class; it is a
 * node holding a data asset, and every node on the canvas is the same class. The
 * class machinery would have had to be worked around rather than used.
 */
UCLASS()
class UARPGAttackTreeGraphSchema : public UEdGraphSchema
{
	GENERATED_BODY()

public:
	virtual void CreateDefaultNodesForGraph(UEdGraph& Graph) const override;
	virtual void GetGraphContextActions(FGraphContextMenuBuilder& ContextMenuBuilder) const override;
	virtual const FPinConnectionResponse CanCreateConnection(const UEdGraphPin* A,
		const UEdGraphPin* B) const override;
	virtual FLinearColor GetPinTypeColor(const FEdGraphPinType& PinType) const override;
	virtual bool ShouldHidePinDefaultValue(UEdGraphPin* Pin) const override { return true; }

	// All three recompile after the base class has done the work. Breaking a
	// wire is an edit like any other, and the asset has to stop believing in a
	// follow-up the moment the canvas does.
	virtual void BreakPinLinks(UEdGraphPin& TargetPin, bool bSendsNodeNotifcation) const override;
	virtual void BreakNodeLinks(UEdGraphNode& TargetNode) const override;
	virtual void BreakSinglePinLink(UEdGraphPin* SourcePin, UEdGraphPin* TargetPin) const override;
};
