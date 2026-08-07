// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGDamageGameplayEffect.h"
#include "ARPGDamageExecution.h"

UARPGDamageGameplayEffect::UARPGDamageGameplayEffect()
{
	// Instant: the execution resolves a number and applies it once. Duration and
	// periodic damage are status effects, which are separate assets (phase 2).
	DurationPolicy = EGameplayEffectDurationType::Instant;

	FGameplayEffectExecutionDefinition ExecutionDef;
	ExecutionDef.CalculationClass = UARPGDamageExecution::StaticClass();
	Executions.Add(ExecutionDef);
}
