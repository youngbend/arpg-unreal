// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGDamageTypeAsset.h"

void UARPGDamageTypeAsset::OnDamageApplied_Implementation(
	AActor* Target, float FinalDamage, const FGameplayEffectContextHandle& Context) const
{
	// Deliberately empty. A plain damage type procs nothing; Blueprint subclasses
	// override this to apply their status effect.
}
