// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGAbilitySystemComponent.h"

UARPGAbilitySystemComponent::UARPGAbilitySystemComponent()
{
	SetIsReplicatedByDefault(true);

	// Mixed is the correct default for a player-owned ASC: the owning client
	// gets full gameplay effect detail (it needs it to predict), while other
	// clients only see replicated tags and attributes. NPCs override this to
	// Minimal where their ASC is constructed -- nobody owns them, so nobody
	// needs the full effect list.
	SetReplicationMode(EGameplayEffectReplicationMode::Mixed);
}
