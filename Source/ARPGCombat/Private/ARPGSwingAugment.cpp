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

bool ARPGSwingAugments::IsContinuationSwing(UObject* Augment,
	const UARPGAttackDefinition* Attack)
{
	if (!Augment || !Attack)
	{
		return true; // nothing supplied this beat, so the weapon's moveset did
	}

	// DERIVED, not stored. This has to agree with the answer the combo component
	// acted on when it chose the beat, and the only way to guarantee that is to
	// ask the same question again rather than to keep a second record of it.
	for (int32 Index = 0; Index < static_cast<int32>(EARPGAttackInput::MAX); ++Index)
	{
		const EARPGAttackInput Input = static_cast<EARPGAttackInput>(Index);
		if (IARPGSwingAugment::Execute_GetSwingAttackOverride(Augment, Input) == Attack)
		{
			return false; // the augment supplied this beat: a specific attack
		}
	}

	return true;
}
