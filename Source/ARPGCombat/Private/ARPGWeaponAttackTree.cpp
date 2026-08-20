// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGWeaponAttackTree.h"

void UARPGWeaponAttackTree::PostLoad()
{
	Super::PostLoad();

	// ONLY when empty. A tree the graph editor has saved lists its nodes
	// explicitly, including ones deliberately left unconnected, and rebuilding
	// over that would delete them the next time the asset loaded.
	if (Nodes.IsEmpty())
	{
		RebuildNodeRegistry();
	}
}

void UARPGWeaponAttackTree::RebuildNodeRegistry()
{
	Nodes.Reset();

	TSet<UARPGComboAttackNode*> Visited;
	TArray<UARPGComboAttackNode*> Pending;

	auto Push = [&Visited, &Pending](UARPGComboAttackNode* Node)
	{
		if (Node && !Visited.Contains(Node))
		{
			Visited.Add(Node);
			Pending.Add(Node);
		}
	};

	for (uint8 Index = 0; Index < static_cast<uint8>(EARPGAttackInput::MAX); ++Index)
	{
		const EARPGAttackInput Input = static_cast<EARPGAttackInput>(Index);
		Push(GetRoot(Input));
		Push(GetParryFollowup(Input));
	}

	// Breadth-first rather than recursive: the visited set already makes cycles
	// safe, and a queue cannot blow the stack on a pathological chain.
	while (!Pending.IsEmpty())
	{
		UARPGComboAttackNode* Node = Pending.Pop(EAllowShrinking::No);
		Nodes.Add(Node);

		for (uint8 Index = 0; Index < static_cast<uint8>(EARPGAttackInput::MAX); ++Index)
		{
			Push(Node->GetFollow(static_cast<EARPGAttackInput>(Index)));
		}
	}
}
