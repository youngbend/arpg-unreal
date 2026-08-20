// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AIGraph.h"
#include "ARPGAttackTreeGraph.generated.h"

class UARPGWeaponAttackTree;

/**
 * The canvas an attack tree is authored on.
 *
 * TWO DIRECTIONS, AND THEY ARE NOT SYMMETRIC. CompileToAsset writes the graph
 * into the asset's arrays and runs on every edit -- the arrays are what the game
 * loads, and a cooked build has no graph at all. RebuildFromAsset goes the other
 * way and runs ONCE, when a tree with no graph is opened: assets built by
 * Tools/build_sword_attack_tree.py, and every tree authored before this editor
 * existed. Without it those open to an empty canvas and the first save wipes a
 * moveset.
 */
UCLASS()
class UARPGAttackTreeGraph : public UAIGraph
{
	GENERATED_BODY()

public:
	UARPGWeaponAttackTree* GetAttackTree() const;

	/** Graph -> asset. The compile step. */
	void CompileToAsset();

	/** Asset -> graph, for a tree that has never been opened here. */
	void RebuildFromAsset();

	/**
	 * Structural edits land here -- a node placed, a wire made or broken.
	 *
	 * Compiling from this one hook rather than from each schema entry point is
	 * what keeps the asset honest: every path that can change the graph ends up
	 * calling it, including undo, paste and delete, none of which the schema
	 * sees.
	 */
	virtual void NotifyGraphChanged() override;

	/** AIGraph's own name for the compile step; the BT and EQS editors call it. */
	virtual void UpdateAsset(int32 UpdateFlags = 0) override;

private:
	/**
	 * True while RebuildFromAsset is building, so the compile step does not run
	 * against a half-wired canvas and write a broken tree back over the one it
	 * is reading from.
	 */
	bool bRebuilding = false;
};
