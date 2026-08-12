// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AIGraphNode.h"
#include "ARPGWeaponAttackTree.h"
#include "ARPGAttackTreeGraphNode.generated.h"

class UARPGWeaponAttackTree;

/** Pin plumbing shared by every node on an attack tree graph. */
namespace ARPGAttackTreeGraph
{
	/** One category for every pin here: these wires carry combo flow, nothing else. */
	extern const FName PinCategory;

	/** The single input pin each attack node has. Several wires may land on it. */
	extern const FName InputPinName;

	/** Output pin names, which ARE the mapping from pin to button. */
	FName FollowPinName(EARPGAttackInput Input);
	FName EntryPinName(EARPGAttackInput Input, bool bParry);

	/**
	 * Reads a pin name back into the input it stands for.
	 *
	 * NAMES RATHER THAN PIN ORDER. Order is what a first version would use, and
	 * it breaks the moment a pin is added: every saved graph silently re-points
	 * its wires by one. A name survives reordering, and an unknown one is a
	 * question the compile step can answer honestly by refusing.
	 */
	bool ParsePinName(FName PinName, EARPGAttackInput& OutInput, bool& bOutParry);
}

UCLASS(Abstract)
class UARPGAttackTreeGraphNodeBase : public UAIGraphNode
{
	GENERATED_BODY()

public:
	/** The asset being edited. The graph is outered to it -- see UARPGAttackTreeGraph. */
	UARPGWeaponAttackTree* GetAttackTree() const;

	/** The pin a follow-up wire leaves from, or null when this node has none. */
	UEdGraphPin* FindOutputPin(EARPGAttackInput Input, bool bParry = false) const;
};

/**
 * The one node you cannot delete: where a combo starts.
 *
 * Six output pins rather than six separate entry nodes, because the asset has
 * exactly six entry points and a canvas that can be missing one of them invites
 * a moveset whose heavy root is simply absent with nothing on screen to say so.
 */
UCLASS()
class UARPGAttackTreeGraphNode_Entry : public UARPGAttackTreeGraphNodeBase
{
	GENERATED_BODY()

public:
	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FLinearColor GetNodeTitleColor() const override;

	virtual bool CanUserDeleteNode() const override { return false; }
	virtual bool CanDuplicateNode() const override { return false; }
};

/** One beat of a combo: an attack, and where each button goes from here. */
UCLASS()
class UARPGAttackTreeGraphNode_Attack : public UARPGAttackTreeGraphNodeBase
{
	GENERATED_BODY()

public:
	/**
	 * The runtime node this box stands for.
	 *
	 * OWNED BY THE ASSET, not by this graph node, and that is the whole
	 * arrangement: the graph is editor-only and stripped from a cooked build,
	 * so anything a runtime combo needs has to live on the asset side of the
	 * line. This is a reference across it, not a container.
	 */
	UPROPERTY()
	TObjectPtr<UARPGComboAttackNode> ComboNode;

	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FLinearColor GetNodeTitleColor() const override;
	virtual void PostPlacedNewNode() override;
	virtual void PostPasteNode() override;

	/** Creates the runtime node if this graph node does not have one yet. */
	void EnsureComboNode();
};
