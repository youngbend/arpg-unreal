// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "StateTreeReference.h"
#include "ARPGNPCDefinition.generated.h"

class UARPGWeaponDefinition;
class USkeletalMesh;

/**
 * What an NPC does when hit by someone it has not perceived.
 *
 * A "flees when hit" NPC needs no third value: set Aggro and author its tree to
 * flee whenever it has a target rather than fight. The aggro plumbing is
 * identical; only the tree's response to HAVING a target differs.
 */
UENUM(BlueprintType)
enum class EARPGDamageReaction : uint8
{
	/**
	 * Walk to where the blow came from and look around. No target, no aggro --
	 * full engagement only follows if the NPC's own perception then genuinely
	 * spots or hears the attacker.
	 */
	Investigate = 0,

	/**
	 * Lock onto the attacker immediately, bypassing sight and hearing entirely.
	 * Being struck reveals the attacker whether or not the NPC had spotted them.
	 */
	Aggro = 1
};

/**
 * One NPC archetype, entirely as data. Port of Godot's NPCDefinition.
 *
 * The whole point is that a new enemy type is an ASSET, not a class: faction,
 * stats, weapon, mesh and behaviour tree all live here, and nothing about adding
 * a goblin requires C++ or a new Blueprint.
 */
UCLASS(BlueprintType)
class ARPGAI_API UARPGNPCDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FName NPCId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FText DisplayName;

	/** Enemy by default, so a newly authored NPC is hostile rather than inert. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity",
		meta = (Categories = "Faction"))
	FGameplayTag FactionTag;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Appearance")
	TSoftObjectPtr<USkeletalMesh> Mesh;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat")
	TObjectPtr<UARPGWeaponDefinition> Weapon;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat",
		meta = (ClampMin = "1.0"))
	float MaxHealth = 100.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat",
		meta = (ClampMin = "0.0"))
	float MaxPoise = 50.f;

	/**
	 * The StateTree this archetype runs.
	 *
	 * A REFERENCE RATHER THAN A BARE UStateTree*, because FStateTreeReference
	 * carries the tree's exposed parameters alongside it -- so an archetype can
	 * reuse one shared tree and override its numbers, which is what stops a
	 * "cautious goblin" needing its own copy of the whole asset.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Behaviour",
		meta = (Schema = "/Script/GameplayStateTree.StateTreeAIComponentSchema"))
	FStateTreeReference StateTreeRef;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Behaviour")
	EARPGDamageReaction DamageReaction = EARPGDamageReaction::Investigate;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement",
		meta = (ClampMin = "0.0"))
	float MoveSpeed = 400.f;

	/**
	 * Degrees per second the NPC pivots.
	 *
	 * ON THE DEFINITION rather than on the face-target node, for the same reason
	 * LeashRange is: how fast a creature turns is a property of the creature. It
	 * is applied to the movement component's RotationRate on possession, which is
	 * what the engine's focus system actually reads -- so the same number governs
	 * a deliberate face-off and any other rotation the pawn does.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement",
		meta = (ClampMin = "0.0"))
	float TurnRateDegrees = 360.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Perception",
		meta = (ClampMin = "0.0"))
	float DetectionRange = 1200.f;

	/**
	 * How far from its spawn the NPC will chase before disengaging.
	 *
	 * On the DEFINITION rather than the tree because it is a property of the
	 * creature -- a guard leashes tight, a hunting beast does not -- and a tree
	 * shared across archetypes should not have to encode either.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Perception",
		meta = (ClampMin = "0.0"))
	float LeashRange = 2500.f;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGNPC", GetFName());
	}
};
