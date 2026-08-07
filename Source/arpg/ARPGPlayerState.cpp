// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGPlayerState.h"
#include "ARPGAbilitySystemComponent.h"

AARPGPlayerState::AARPGPlayerState()
{
	AbilitySystemComponent = CreateDefaultSubobject<UARPGAbilitySystemComponent>(
		TEXT("AbilitySystemComponent"));

	// A PlayerState replicates at 1 Hz by default, which is fine for a score
	// but visibly wrong for a health bar -- attribute changes would arrive up to
	// a second late. Phase 1's attribute sets live on this actor, so raise it
	// here rather than discovering the lag once there is something to see.
	SetNetUpdateFrequency(100.f);
}

UAbilitySystemComponent* AARPGPlayerState::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}
