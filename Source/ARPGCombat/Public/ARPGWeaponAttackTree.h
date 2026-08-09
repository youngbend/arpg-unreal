// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ARPGWeaponAttackTree.generated.h"

class UARPGAttackDefinition;
class UARPGBlockDefinition;
class UARPGFlinchDefinition;

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
 * Instanced, so the whole tree nests inside a single asset the way Godot's
 * sub-resources nested inside one .tres -- rather than scattering one asset per
 * beat across the content browser.
 */
UCLASS(EditInlineNew, DefaultToInstanced, BlueprintType)
class ARPGCOMBAT_API UARPGComboAttackNode : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combo")
	TObjectPtr<UARPGAttackDefinition> Attack;

	UPROPERTY(EditAnywhere, Instanced, BlueprintReadOnly, Category = "Combo")
	TObjectPtr<UARPGComboAttackNode> FollowLight;

	UPROPERTY(EditAnywhere, Instanced, BlueprintReadOnly, Category = "Combo")
	TObjectPtr<UARPGComboAttackNode> FollowHeavy;

	UPROPERTY(EditAnywhere, Instanced, BlueprintReadOnly, Category = "Combo")
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

	/**
	 * True when this node terminates every branch it is reached through.
	 *
	 * A POSITION IN THE TREE, not a property of the attack. The same
	 * UARPGAttackDefinition can sit on a leaf in one branch and mid-chain in
	 * another, so "is this a finisher" is answered here rather than by a flag on
	 * the attack -- which is why FinisherLockout only takes effect on leaves.
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
	UPROPERTY(EditDefaultsOnly, Instanced, BlueprintReadOnly, Category = "Roots")
	TObjectPtr<UARPGComboAttackNode> RootLight;

	UPROPERTY(EditDefaultsOnly, Instanced, BlueprintReadOnly, Category = "Roots")
	TObjectPtr<UARPGComboAttackNode> RootHeavy;

	UPROPERTY(EditDefaultsOnly, Instanced, BlueprintReadOnly, Category = "Roots")
	TObjectPtr<UARPGComboAttackNode> RootSpecial;

	/** Used instead of the root when attacking straight out of a successful parry. */
	UPROPERTY(EditDefaultsOnly, Instanced, BlueprintReadOnly, Category = "Parry Follow-up")
	TObjectPtr<UARPGComboAttackNode> ParryLight;

	UPROPERTY(EditDefaultsOnly, Instanced, BlueprintReadOnly, Category = "Parry Follow-up")
	TObjectPtr<UARPGComboAttackNode> ParryHeavy;

	UPROPERTY(EditDefaultsOnly, Instanced, BlueprintReadOnly, Category = "Parry Follow-up")
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

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGAttackTree", GetFName());
	}
};
