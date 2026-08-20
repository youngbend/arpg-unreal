// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGPlayerState.h"
#include "ARPGAbilitySystemComponent.h"
#include "ARPGGameplayTags.h"
#include "ARPGOffenseSet.h"
#include "ARPGResistanceSet.h"
#include "ARPGVitalSet.h"

AARPGPlayerState::AARPGPlayerState()
{
	// Mixed: this ASC is owned and predicted for by exactly one client.
	Combatant.Create(*this, EGameplayEffectReplicationMode::Mixed);

	// A PlayerState replicates at 1 Hz by default, which is fine for a score
	// but visibly wrong for a health bar -- attribute changes would arrive up to
	// a second late.
	SetNetUpdateFrequency(100.f);
}

UAbilitySystemComponent* AARPGPlayerState::GetAbilitySystemComponent() const
{
	return Combatant.AbilitySystem;
}

void AARPGPlayerState::BeginPlay()
{
	Super::BeginPlay();

	// Faction is applied server-side and replicated with TagOnly so clients can
	// tint nameplates and suppress friendly-fire feedback. The authoritative
	// filtering still happens on the server (see UARPGHitboxComponent).
	if (HasAuthority() && Combatant.AbilitySystem)
	{
		Combatant.AbilitySystem->AddLooseGameplayTag(
			TAG_Faction_Player, 1, EGameplayTagReplicationState::TagOnly);
	}
}
