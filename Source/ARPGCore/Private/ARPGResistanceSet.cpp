// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGResistanceSet.h"
#include "Net/UnrealNetwork.h"

UARPGResistanceSet::UARPGResistanceSet()
{
	InitBaseArmor(0.f);

	// 0 == takes full damage from everything, which is the right neutral
	// default: an archetype opts into resistance, it does not opt out.
	InitPhysicalResistance(0.f);
	InitFireResistance(0.f);
	InitIceResistance(0.f);
	InitLightResistance(0.f);
	InitLightningResistance(0.f);
	InitWaterResistance(0.f);
	InitFallResistance(0.f);
}

void UARPGResistanceSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION_NOTIFY(UARPGResistanceSet, BaseArmor,           COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGResistanceSet, PhysicalResistance,  COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGResistanceSet, FireResistance,      COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGResistanceSet, IceResistance,       COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGResistanceSet, LightResistance,     COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGResistanceSet, LightningResistance, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGResistanceSet, WaterResistance,     COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGResistanceSet, FallResistance,      COND_None, REPNOTIFY_Always);
}

void UARPGResistanceSet::OnRep_BaseArmor(const FGameplayAttributeData& OldValue)           { ARPG_REPNOTIFY(UARPGResistanceSet, BaseArmor, OldValue); }
void UARPGResistanceSet::OnRep_PhysicalResistance(const FGameplayAttributeData& OldValue)  { ARPG_REPNOTIFY(UARPGResistanceSet, PhysicalResistance, OldValue); }
void UARPGResistanceSet::OnRep_FireResistance(const FGameplayAttributeData& OldValue)      { ARPG_REPNOTIFY(UARPGResistanceSet, FireResistance, OldValue); }
void UARPGResistanceSet::OnRep_IceResistance(const FGameplayAttributeData& OldValue)       { ARPG_REPNOTIFY(UARPGResistanceSet, IceResistance, OldValue); }
void UARPGResistanceSet::OnRep_LightResistance(const FGameplayAttributeData& OldValue)     { ARPG_REPNOTIFY(UARPGResistanceSet, LightResistance, OldValue); }
void UARPGResistanceSet::OnRep_LightningResistance(const FGameplayAttributeData& OldValue) { ARPG_REPNOTIFY(UARPGResistanceSet, LightningResistance, OldValue); }
void UARPGResistanceSet::OnRep_WaterResistance(const FGameplayAttributeData& OldValue)     { ARPG_REPNOTIFY(UARPGResistanceSet, WaterResistance, OldValue); }
void UARPGResistanceSet::OnRep_FallResistance(const FGameplayAttributeData& OldValue)      { ARPG_REPNOTIFY(UARPGResistanceSet, FallResistance, OldValue); }
