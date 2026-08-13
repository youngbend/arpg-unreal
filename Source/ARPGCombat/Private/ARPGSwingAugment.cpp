// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGSwingAugment.h"
#include "Abilities/GameplayAbility.h"
#include "AbilitySystemComponent.h"

UObject* ARPGSwingAugments::FindActive(UAbilitySystemComponent* ASC)
{
	if (!ASC)
	{
		return nullptr;
	}

	for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
	{
		if (!Spec.IsActive())
		{
			continue;
		}

		// Instances rather than Spec.Ability: the CDO is not what holds the
		// augment's state, and an InstancedPerActor ability like the imbue keeps
		// exactly one live instance that does.
		for (UGameplayAbility* Instance : Spec.GetAbilityInstances())
		{
			if (Instance && Instance->GetClass()->ImplementsInterface(UARPGSwingAugment::StaticClass()))
			{
				return Instance;
			}
		}
	}

	return nullptr;
}
