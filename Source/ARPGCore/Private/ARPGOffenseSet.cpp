// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGOffenseSet.h"
#include "Net/UnrealNetwork.h"

UARPGOffenseSet::UARPGOffenseSet()
{
	InitAttackPower(0.f);
	InitCritChance(0.f);
	InitCritMultiplier(2.f);
	InitDamageAmpMultiplier(1.f); // identity -- see the header
}

void UARPGOffenseSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION_NOTIFY(UARPGOffenseSet, AttackPower,         COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGOffenseSet, CritChance,          COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGOffenseSet, CritMultiplier,      COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGOffenseSet, DamageAmpMultiplier, COND_None, REPNOTIFY_Always);
}

void UARPGOffenseSet::OnRep_AttackPower(const FGameplayAttributeData& OldValue)         { ARPG_REPNOTIFY(UARPGOffenseSet, AttackPower, OldValue); }
void UARPGOffenseSet::OnRep_CritChance(const FGameplayAttributeData& OldValue)          { ARPG_REPNOTIFY(UARPGOffenseSet, CritChance, OldValue); }
void UARPGOffenseSet::OnRep_CritMultiplier(const FGameplayAttributeData& OldValue)      { ARPG_REPNOTIFY(UARPGOffenseSet, CritMultiplier, OldValue); }
void UARPGOffenseSet::OnRep_DamageAmpMultiplier(const FGameplayAttributeData& OldValue) { ARPG_REPNOTIFY(UARPGOffenseSet, DamageAmpMultiplier, OldValue); }
