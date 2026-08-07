// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemGlobals.h"
#include "ARPGAbilitySystemGlobals.generated.h"

/**
 * Exists for one reason: to hand out FARPGGameplayEffectContext instead of the
 * stock FGameplayEffectContext, so every effect spec in the game carries our
 * poise/knockback/hit-stop payload.
 *
 * Must be registered in DefaultGame.ini or it is never constructed and every
 * context silently falls back to the base type:
 *
 *   [/Script/GameplayAbilities.AbilitySystemGlobals]
 *   AbilitySystemGlobalsClassName="/Script/ARPGCore.ARPGAbilitySystemGlobals"
 */
UCLASS()
class ARPGCORE_API UARPGAbilitySystemGlobals : public UAbilitySystemGlobals
{
	GENERATED_BODY()

public:
	virtual FGameplayEffectContext* AllocGameplayEffectContext() const override;
};
