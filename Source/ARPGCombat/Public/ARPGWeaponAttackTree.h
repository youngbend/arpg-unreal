// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ARPGWeaponAttackTree.generated.h"

class UARPGAttackDefinition;
class UARPGBlockDefinition;
class UARPGFlinchDefinition;
class UEdGraph;

/** Which button drove the input. */
UENUM(BlueprintType)
enum class EARPGAttackInput : uint8
{
	Light,
	Heavy,
	Special,
	MAX UMETA(Hidden)
};

/**
 * One node in a weapon's branching combo tree.
 *
 * FLAT, NOT NESTED. The follow-ups used to be Instanced subobjects, so the tree
 * physically contained its children and the whole moveset serialised inside one
 * asset the way Godot's sub-resources nested inside one .tres. That shape cannot
 * express a node with two parents: UE instances a fresh copy per reference, so
 * the three shared finishers in the sword tree became nine copies, and editing
 * one no longer edited the others.
 *
 * Now the tree owns a flat array and nodes reference each other plainly. Sharing
 * works, a chain may loop back on itself, and -- the reason this changed -- a
 * node graph can round-trip the structure, which a tree of owned subobjects
 * cannot.
 *
 * STILL A UOBJECT rather than a struct: the combo component holds the current
 * node by pointer across frames, and OnAttackStarted hands it to Blueprints.
 */
UCLASS(EditInlineNew, BlueprintType)
class ARPGCOMBAT_API UARPGComboAttackNode : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combo")
	TObjectPtr<UARPGAttackDefinition> Attack;

	// Plain references into the owning tree's Nodes array -- see the class note.
	// A null follow means this input ends the chain here and falls back to root.

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combo")
	TObjectPtr<UARPGComboAttackNode> FollowLight;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combo")
	TObjectPtr<UARPGComboAttackNode> FollowHeavy;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combo")
	TObjectPtr<UARPGComboAttackNode> FollowSpecial;

	/** Overrides the tree-level reset timeout for this node. 0 = use the tree's. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = "0.0"))
	float ResetTimeoutOverride = 0.f;

	UARPGComboAttackNode* GetFollow(EARPGAttackInput Input) const
	{
		switch (Input)
		{
		case EARPGAttackInput::Light:   return FollowLight;
		case EARPGAttackInput::Heavy:   return FollowHeavy;
		case EARPGAttackInput::Special: return FollowSpecial;
		default: return nullptr;
		}
	}

	/** The write half of GetFollow. Used by the graph editor's compile step. */
	void SetFollow(EARPGAttackInput Input, UARPGComboAttackNode* Node)
	{
		switch (Input)
		{
		case EARPGAttackInput::Light:   FollowLight = Node;   break;
		case EARPGAttackInput::Heavy:   FollowHeavy = Node;   break;
		case EARPGAttackInput::Special: FollowSpecial = Node; break;
		default: break;
		}
	}

	/**
	 * True when this node terminates every branch it is reached through.
	 *
	 * A POSITION IN THE TREE, not a property of the attack. The same
	 * UARPGAttackDefinition can sit on a leaf in one branch and mid-chain in
	 * another, so "is this a finisher" is answered here rather than by a flag on
	 * the attack -- which is why FinisherLockout only takes effect on leaves.
	 *
	 * Now that nodes are shared rather than copied, this is answered once for the
	 * one node instead of per-copy: a finisher reached from three branches is a
	 * leaf in all three, which is what it always meant.
	 */
	bool IsLeaf() const
	{
		return !FollowLight && !FollowHeavy && !FollowSpecial;
	}
};

/** A weapon's complete moveset: grounded combo trees, air attacks, and parry follow-ups. */
UCLASS(BlueprintType)
class ARPGCOMBAT_API UARPGWeaponAttackTree : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	virtual void PostLoad() override;

	/**
	 * Every node this tree owns, connected or not.
	 *
	 * The graph editor rewrites this wholesale on every change, and a node not
	 * reachable from any entry point is kept deliberately -- that is a node you
	 * have placed and not yet wired up, and dropping it would delete work as you
	 * did it.
	 *
	 * Editable rather than read-only so Tools/build_sword_attack_tree.py can fill
	 * it: the Python editor API refuses to write VisibleAnywhere properties. Hand
	 * editing here is legal but pointless -- open the graph.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Nodes")
	TArray<TObjectPtr<UARPGComboAttackNode>> Nodes;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Roots")
	TObjectPtr<UARPGComboAttackNode> RootLight;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Roots")
	TObjectPtr<UARPGComboAttackNode> RootHeavy;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Roots")
	TObjectPtr<UARPGComboAttackNode> RootSpecial;

	/** Used instead of the root when attacking straight out of a successful parry. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Parry Follow-up")
	TObjectPtr<UARPGComboAttackNode> ParryLight;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Parry Follow-up")
	TObjectPtr<UARPGComboAttackNode> ParryHeavy;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Parry Follow-up")
	TObjectPtr<UARPGComboAttackNode> ParrySpecial;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Air")
	TObjectPtr<UARPGAttackDefinition> AirLight;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Air")
	TObjectPtr<UARPGAttackDefinition> AirHeavy;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Air")
	TObjectPtr<UARPGAttackDefinition> AirSpecial;

	/**
	 * Block and parry timings for this weapon, and the clips that go with them.
	 *
	 * Null means this weapon CANNOT block, which is the honest default: a bow
	 * has no guard to raise. The parry component falls back to its own settings
	 * so an unarmed or unauthored character still behaves sanely.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Defence")
	TObjectPtr<UARPGBlockDefinition> Block;

	/** Poise reactions played while holding this weapon. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Defence")
	TObjectPtr<UARPGFlinchDefinition> Flinch;

	/** Seconds at rest before the chain returns to root. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = "0.0"))
	float ResetTimeout = 1.5f;

#if WITH_EDITORONLY_DATA
	/**
	 * The node graph this tree was authored in. Editor-only, and NOT the source
	 * of truth at runtime -- the arrays above are, and they are what a cooked
	 * build ships. The graph compiles into them on every edit.
	 *
	 * Null on a tree built by Tools/build_sword_attack_tree.py, or authored
	 * before the editor existed. Opening one rebuilds a graph from the arrays
	 * rather than showing an empty canvas; see UARPGAttackTreeGraph.
	 */
	UPROPERTY()
	TObjectPtr<UEdGraph> EdGraph;
#endif

	UARPGComboAttackNode* GetRoot(EARPGAttackInput Input) const
	{
		switch (Input)
		{
		case EARPGAttackInput::Light:   return RootLight;
		case EARPGAttackInput::Heavy:   return RootHeavy;
		case EARPGAttackInput::Special: return RootSpecial;
		default: return nullptr;
		}
	}

	void SetRoot(EARPGAttackInput Input, UARPGComboAttackNode* Node)
	{
		switch (Input)
		{
		case EARPGAttackInput::Light:   RootLight = Node;   break;
		case EARPGAttackInput::Heavy:   RootHeavy = Node;   break;
		case EARPGAttackInput::Special: RootSpecial = Node; break;
		default: break;
		}
	}

	UARPGComboAttackNode* GetParryFollowup(EARPGAttackInput Input) const
	{
		switch (Input)
		{
		case EARPGAttackInput::Light:   return ParryLight;
		case EARPGAttackInput::Heavy:   return ParryHeavy;
		case EARPGAttackInput::Special: return ParrySpecial;
		default: return nullptr;
		}
	}

	void SetParryFollowup(EARPGAttackInput Input, UARPGComboAttackNode* Node)
	{
		switch (Input)
		{
		case EARPGAttackInput::Light:   ParryLight = Node;   break;
		case EARPGAttackInput::Heavy:   ParryHeavy = Node;   break;
		case EARPGAttackInput::Special: ParrySpecial = Node; break;
		default: break;
		}
	}

	UARPGAttackDefinition* GetAirAttack(EARPGAttackInput Input) const
	{
		switch (Input)
		{
		case EARPGAttackInput::Light:   return AirLight;
		case EARPGAttackInput::Heavy:   return AirHeavy;
		case EARPGAttackInput::Special: return AirSpecial;
		default: return nullptr;
		}
	}

	/**
	 * Refills Nodes by walking every entry point, for a tree that has references
	 * but no registry: an asset saved before the array existed, or one a script
	 * built by wiring roots and follow-ups directly.
	 *
	 * CYCLE-SAFE, because a chain looping back to its own opener is a legal
	 * moveset now that nodes can be shared -- the combo component resolves one
	 * step per press and never walks the structure, so nothing at runtime can
	 * recurse. This is the only code that walks it, and it carries the visited
	 * set that makes that safe.
	 */
	void RebuildNodeRegistry();

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGAttackTree", GetFName());
	}
};
