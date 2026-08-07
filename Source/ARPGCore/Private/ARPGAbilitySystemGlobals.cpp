// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGAbilitySystemGlobals.h"
#include "ARPGGameplayEffectContext.h"

FGameplayEffectContext* UARPGAbilitySystemGlobals::AllocGameplayEffectContext() const
{
	return new FARPGGameplayEffectContext();
}
