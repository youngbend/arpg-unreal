// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGTestSwingAugment.h"

UARPGTestSwingAugment::UARPGTestSwingAugment()
{
	// InstancedPerActor to match the real augment: FindActive walks instances,
	// and LocalOnly so activation needs no prediction key or net connection in a
	// bare test world.
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalOnly;
}

void UARPGTestSwingAugment::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	// Deliberately does not end. The augment IS the readied coating, and it has
	// to still be active when the next swing asks about it.
}

UARPGAttackDefinition* UARPGTestSwingAugment::GetSwingAttackOverride_Implementation(
	EARPGAttackInput Input)
{
	switch (Input)
	{
	case EARPGAttackInput::Light:   return OverrideLight;
	case EARPGAttackInput::Heavy:   return OverrideHeavy;
	case EARPGAttackInput::Special: return OverrideSpecial;
	default:                        return nullptr;
	}
}

void UARPGTestSwingAugment::ArmSwingAugment_Implementation(UARPGHitboxComponent* SwingHitbox,
	float MotionValue)
{
	++ArmCount;
	LastMotionValue = MotionValue;
}

void UARPGTestSwingAugment::DisarmSwingAugment_Implementation()
{
	++DisarmCount;
}

void UARPGTestSwingAugment::NotifySwingEnded_Implementation()
{
	++SwingEndedCount;
}
