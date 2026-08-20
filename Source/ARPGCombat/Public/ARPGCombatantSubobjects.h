// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGCombatantSubobjects.generated.h"

// Forward-declared rather than pulling in AbilitySystemComponent.h, which is one
// of the heaviest headers in GAS. The enum has a fixed underlying type, so the
// declaration alone is enough for a by-value parameter.
enum class EGameplayEffectReplicationMode : uint8;

class AActor;
class UARPGAbilitySystemComponent;
class UARPGOffenseSet;
class UARPGResistanceSet;
class UARPGVitalSet;

/**
 * The ability system component and the three attribute sets every damageable
 * actor in this game owns, created together.
 *
 * WHY THIS IS A STRUCT AND NOT A COMPONENT. GAS discovers attribute sets by
 * scanning the objects whose OUTER is the ASC's owning actor -- and the scan in
 * UAbilitySystemComponent::InitializeComponent is deliberately non-recursive.
 * An attribute set created inside a component has that component as its outer,
 * so it is never found, never registered, and every attribute on it silently
 * reads zero. Folding these four into a UActorComponent is therefore the one
 * refactor that looks tidiest and breaks the most.
 *
 * A struct sidesteps that entirely. CreateDefaultSubobject is public on UObject,
 * so Create() below calls it ON THE OWNING ACTOR: the outer is exactly what it
 * was when each actor wrote these four lines itself, GAS's discovery is
 * untouched, and what actually collapses is the duplication -- three actors
 * (AARPGPlayerState, AARPGNPCCharacter, AARPGCombatDummy) had four identical
 * declarations and four identical constructor lines each, with the replication
 * mode reasoning copy-pasted verbatim between two of them.
 *
 * SUBOBJECT NAMES ARE LOAD-BEARING and unchanged. All three actors already used
 * "AbilitySystemComponent", "VitalSet", "OffenseSet" and "ResistanceSet";
 * renaming any of them would orphan the corresponding subobject in every saved
 * Blueprint and placed instance.
 */
USTRUCT(BlueprintType)
struct ARPGCOMBAT_API FARPGCombatantSubobjects
{
	GENERATED_BODY()

	/**
	 * Creates all four subobjects on Owner. Call from the owner's CONSTRUCTOR --
	 * CreateDefaultSubobject is only legal there.
	 *
	 * @param ReplicationMode  Mixed for anything a client owns and predicts for;
	 *                         Minimal for anything nobody owns. See the comment
	 *                         on the parameter's use below, which is the single
	 *                         copy of an argument that used to live in two
	 *                         actors at once.
	 */
	void Create(AActor& Owner, EGameplayEffectReplicationMode ReplicationMode);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ARPG|Abilities")
	TObjectPtr<UARPGAbilitySystemComponent> AbilitySystem;

	UPROPERTY()
	TObjectPtr<UARPGVitalSet> VitalSet;

	UPROPERTY()
	TObjectPtr<UARPGOffenseSet> OffenseSet;

	UPROPERTY()
	TObjectPtr<UARPGResistanceSet> ResistanceSet;
};
