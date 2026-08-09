// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGAbilitySystemComponent.h"
#include "ARPGStatusVfxSubsystem.h"
#include "Engine/World.h"

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

void UARPGAbilitySystemComponent::InitializeComponent()
{
	Super::InitializeComponent();

	if (const UWorld* World = GetWorld())
	{
		if (UARPGStatusVfxSubsystem* Vfx = World->GetSubsystem<UARPGStatusVfxSubsystem>())
		{
			Vfx->RegisterAbilitySystem(this);
		}
	}
}

void UARPGAbilitySystemComponent::UninitializeComponent()
{
	if (const UWorld* World = GetWorld())
	{
		if (UARPGStatusVfxSubsystem* Vfx = World->GetSubsystem<UARPGStatusVfxSubsystem>())
		{
			Vfx->UnregisterAbilitySystem(this);
		}
	}

	Super::UninitializeComponent();
}
