// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGCombatantSubobjects.h"
#include "ARPGAbilitySystemComponent.h"
#include "ARPGOffenseSet.h"
#include "ARPGResistanceSet.h"
#include "ARPGVitalSet.h"
#include "GameFramework/Actor.h"

void FARPGCombatantSubobjects::Create(AActor& Owner, EGameplayEffectReplicationMode ReplicationMode)
{
	AbilitySystem = Owner.CreateDefaultSubobject<UARPGAbilitySystemComponent>(
		TEXT("AbilitySystemComponent"));

	// MIXED for anything a client owns and predicts for: the owning client gets
	// full gameplay-effect detail, other clients see only replicated tags and
	// attributes. MINIMAL for anything nobody owns -- an NPC or a training
	// dummy -- because no client needs its effect list at all. Port plan §3.1.
	//
	// Passed in rather than inferred: an actor knows whether it is owned, and
	// guessing from the class would make the training dummy and the NPC agree by
	// coincidence rather than by decision.
	AbilitySystem->SetReplicationMode(ReplicationMode);

	// Attribute sets are subobjects of the ASC's OWNING ACTOR, which is how GAS
	// discovers and registers them -- see the header for why that rules out
	// creating them inside a component. Creating them anywhere else means they
	// never get registered and every attribute reads as 0.
	VitalSet      = Owner.CreateDefaultSubobject<UARPGVitalSet>(TEXT("VitalSet"));
	OffenseSet    = Owner.CreateDefaultSubobject<UARPGOffenseSet>(TEXT("OffenseSet"));
	ResistanceSet = Owner.CreateDefaultSubobject<UARPGResistanceSet>(TEXT("ResistanceSet"));
}
