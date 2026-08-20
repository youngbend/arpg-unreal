// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGAttackTreeGraphNode.h"
#include "ARPGAttackDefinition.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"

#define LOCTEXT_NAMESPACE "ARPGAttackTreeGraph"

namespace ARPGAttackTreeGraph
{
	const FName PinCategory("ARPGCombo");
	const FName InputPinName("In");

	static const FName LightPin("Light");
	static const FName HeavyPin("Heavy");
	static const FName SpecialPin("Special");
	static const FName ParryLightPin("ParryLight");
	static const FName ParryHeavyPin("ParryHeavy");
	static const FName ParrySpecialPin("ParrySpecial");

	FName FollowPinName(EARPGAttackInput Input)
	{
		switch (Input)
		{
		case EARPGAttackInput::Light:   return LightPin;
		case EARPGAttackInput::Heavy:   return HeavyPin;
		case EARPGAttackInput::Special: return SpecialPin;
		default: return NAME_None;
		}
	}

	FName EntryPinName(EARPGAttackInput Input, bool bParry)
	{
		if (!bParry)
		{
			return FollowPinName(Input);
		}

		switch (Input)
		{
		case EARPGAttackInput::Light:   return ParryLightPin;
		case EARPGAttackInput::Heavy:   return ParryHeavyPin;
		case EARPGAttackInput::Special: return ParrySpecialPin;
		default: return NAME_None;
		}
	}

	bool ParsePinName(FName PinName, EARPGAttackInput& OutInput, bool& bOutParry)
	{
		bOutParry = false;

		if (PinName == LightPin)        { OutInput = EARPGAttackInput::Light;   return true; }
		if (PinName == HeavyPin)        { OutInput = EARPGAttackInput::Heavy;   return true; }
		if (PinName == SpecialPin)      { OutInput = EARPGAttackInput::Special; return true; }

		bOutParry = true;

		if (PinName == ParryLightPin)   { OutInput = EARPGAttackInput::Light;   return true; }
		if (PinName == ParryHeavyPin)   { OutInput = EARPGAttackInput::Heavy;   return true; }
		if (PinName == ParrySpecialPin) { OutInput = EARPGAttackInput::Special; return true; }

		return false;
	}
}

// ---------------------------------------------------------------------------

UARPGWeaponAttackTree* UARPGAttackTreeGraphNodeBase::GetAttackTree() const
{
	const UEdGraph* Graph = GetGraph();
	return Graph ? Cast<UARPGWeaponAttackTree>(Graph->GetOuter()) : nullptr;
}

UEdGraphPin* UARPGAttackTreeGraphNodeBase::FindOutputPin(EARPGAttackInput Input, bool bParry) const
{
	const FName Wanted = ARPGAttackTreeGraph::EntryPinName(Input, bParry);
	for (UEdGraphPin* Pin : Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinName == Wanted)
		{
			return Pin;
		}
	}
	return nullptr;
}

// --- Entry -----------------------------------------------------------------

void UARPGAttackTreeGraphNode_Entry::AllocateDefaultPins()
{
	using namespace ARPGAttackTreeGraph;

	// Roots first, then the parry follow-ups, so the six read top to bottom in
	// the order the combo component checks them.
	static const TCHAR* const RootLabels[]  = { TEXT("Light"), TEXT("Heavy"), TEXT("Special") };
	static const TCHAR* const ParryLabels[] = { TEXT("Parry Light"), TEXT("Parry Heavy"),
	                                            TEXT("Parry Special") };

	for (int32 Index = 0; Index < static_cast<int32>(EARPGAttackInput::MAX); ++Index)
	{
		const EARPGAttackInput Input = static_cast<EARPGAttackInput>(Index);
		UEdGraphPin* Pin = CreatePin(EGPD_Output, PinCategory, EntryPinName(Input, /*bParry=*/false));
		Pin->PinFriendlyName = FText::FromString(RootLabels[Index]);
	}

	for (int32 Index = 0; Index < static_cast<int32>(EARPGAttackInput::MAX); ++Index)
	{
		const EARPGAttackInput Input = static_cast<EARPGAttackInput>(Index);
		UEdGraphPin* Pin = CreatePin(EGPD_Output, PinCategory, EntryPinName(Input, /*bParry=*/true));
		Pin->PinFriendlyName = FText::FromString(ParryLabels[Index]);
	}
}

FText UARPGAttackTreeGraphNode_Entry::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("EntryTitle", "Combo Start");
}

FText UARPGAttackTreeGraphNode_Entry::GetTooltipText() const
{
	return LOCTEXT("EntryTooltip",
		"Where a chain begins. The top three pins are the roots each button opens with; "
		"the parry pins are taken instead when the attack comes straight out of a "
		"successful parry.");
}

FLinearColor UARPGAttackTreeGraphNode_Entry::GetNodeTitleColor() const
{
	return FLinearColor(0.1f, 0.35f, 0.12f);
}

// --- Attack ----------------------------------------------------------------

void UARPGAttackTreeGraphNode_Attack::AllocateDefaultPins()
{
	using namespace ARPGAttackTreeGraph;

	// ONE input pin taking any number of wires. That is what lets three branches
	// end on the same finisher -- the thing the old nested-subobject asset could
	// not express, and copied the node three times to fake.
	UEdGraphPin* In = CreatePin(EGPD_Input, PinCategory, InputPinName);
	In->PinFriendlyName = FText::GetEmpty();

	static const TCHAR* const Labels[] = { TEXT("Light"), TEXT("Heavy"), TEXT("Special") };
	for (int32 Index = 0; Index < static_cast<int32>(EARPGAttackInput::MAX); ++Index)
	{
		const EARPGAttackInput Input = static_cast<EARPGAttackInput>(Index);
		UEdGraphPin* Pin = CreatePin(EGPD_Output, PinCategory, FollowPinName(Input));
		Pin->PinFriendlyName = FText::FromString(Labels[Index]);
	}
}

FText UARPGAttackTreeGraphNode_Attack::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	const UARPGAttackDefinition* Attack = ComboNode ? ComboNode->Attack : nullptr;
	if (!Attack)
	{
		// Named for what it is rather than left blank: a node with no attack is
		// the one authoring mistake the combo component cannot recover from, and
		// it warns about at runtime. Say so on the canvas instead.
		return LOCTEXT("AttackTitleEmpty", "(no attack assigned)");
	}

	if (!Attack->DisplayName.IsEmpty())
	{
		return Attack->DisplayName;
	}

	return FText::FromName(Attack->AttackId);
}

FText UARPGAttackTreeGraphNode_Attack::GetTooltipText() const
{
	const UARPGAttackDefinition* Attack = ComboNode ? ComboNode->Attack : nullptr;
	if (!Attack)
	{
		return LOCTEXT("AttackTooltipEmpty",
			"No attack assigned. This beat resolves to nothing and the combo stalls here.");
	}

	return FText::Format(
		LOCTEXT("AttackTooltip", "{0}\nStamina {1}  |  Motion value {2}{3}"),
		FText::FromName(Attack->AttackId),
		FText::AsNumber(Attack->StaminaCost),
		FText::AsNumber(Attack->MotionValue),
		Attack->Montage ? FText::GetEmpty() : LOCTEXT("NoMontage", "\nNO MONTAGE -- nothing will play."));
}

FLinearColor UARPGAttackTreeGraphNode_Attack::GetNodeTitleColor() const
{
	// Red for a node with no attack. The canvas is where that is cheapest to
	// notice, and it costs nothing to say it in colour as well as in the title.
	return (ComboNode && ComboNode->Attack)
		? FLinearColor(0.08f, 0.2f, 0.4f)
		: FLinearColor(0.45f, 0.1f, 0.1f);
}

void UARPGAttackTreeGraphNode_Attack::PostPlacedNewNode()
{
	Super::PostPlacedNewNode();
	EnsureComboNode();
}

void UARPGAttackTreeGraphNode_Attack::PostPasteNode()
{
	Super::PostPasteNode();

	// A pasted graph node arrives still pointing at the ORIGINAL's runtime node,
	// because the pointer is a plain reference and copying a graph node copies
	// the reference rather than what it names. Left alone, editing the pasted
	// box would edit the box it was copied from.
	UARPGComboAttackNode* Source = ComboNode;
	ComboNode = nullptr;
	EnsureComboNode();

	if (Source && ComboNode)
	{
		ComboNode->Attack = Source->Attack;
		ComboNode->ResetTimeoutOverride = Source->ResetTimeoutOverride;
		// Follow-ups are deliberately not copied: they are rebuilt from the
		// pasted node's own wires the next time the graph compiles.
	}
}

void UARPGAttackTreeGraphNode_Attack::EnsureComboNode()
{
	if (ComboNode)
	{
		return;
	}

	if (UARPGWeaponAttackTree* Tree = GetAttackTree())
	{
		ComboNode = NewObject<UARPGComboAttackNode>(Tree, NAME_None, RF_Transactional);
	}
}

#undef LOCTEXT_NAMESPACE
