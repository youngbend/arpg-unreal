// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGStatusApplicationComponent.h"
#include "ARPGStatusEffectComponent.h"
#include "ARPGStatusResistanceComponent.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"

UARPGStatusApplicationComponent::UARPGStatusApplicationComponent()
{
#if WITH_EDITORONLY_DATA
	EditorFriendlyName = TEXT("ARPG Status Application");
#endif
}

bool UARPGStatusApplicationComponent::CanGameplayEffectApply(
	const FActiveGameplayEffectsContainer& ActiveGEContainer,
	const FGameplayEffectSpec& GESpec) const
{
	const UAbilitySystemComponent* TargetASC = ActiveGEContainer.Owner;
	if (!TargetASC)
	{
		return true;
	}

	if (bRequireTargetAlive)
	{
		bool bFound = false;
		const float Health = TargetASC->GetGameplayAttributeValue(
			UARPGVitalSet::GetHealthAttribute(), bFound);

		// Only block when health was actually readable. A target with no vital
		// set is something like a destructible with its own rules, not a corpse.
		if (bFound && Health <= 0.f)
		{
			return false;
		}
	}

	if (!bRespectTargetResistance)
	{
		return true;
	}

	// Resolve which status this effect is, from its sibling component. Without a
	// StatusTag there is nothing to look resistance up by.
	const UARPGStatusEffectComponent* StatusInfo =
		GetOwner() ? GetOwner()->FindComponent<UARPGStatusEffectComponent>() : nullptr;
	if (!StatusInfo || !StatusInfo->StatusTag.IsValid())
	{
		return true;
	}

	const AActor* TargetActor = TargetASC->GetAvatarActor_Direct();
	if (!TargetActor)
	{
		TargetActor = TargetASC->GetOwnerActor();
	}

	const UARPGStatusResistanceComponent* Resistance =
		TargetActor ? TargetActor->FindComponentByClass<UARPGStatusResistanceComponent>() : nullptr;
	if (!Resistance)
	{
		return true;
	}

	if (Resistance->IsImmuneTo(StatusInfo->StatusTag))
	{
		return false;
	}

	const float ResistChance = Resistance->GetResistance(StatusInfo->StatusTag);
	if (ResistChance <= 0.f)
	{
		return true;
	}

	// Rolled on the server only. This runs during client prediction too, and an
	// independent client roll would show a status flickering on and then being
	// yanked away by replication. Letting the client through means the worst
	// case is a status that briefly appears and is then corrected, which is the
	// normal prediction failure mode rather than a bespoke one.
	if (!TargetASC->IsOwnerActorAuthoritative())
	{
		return true;
	}

	return FMath::FRand() >= ResistChance;
}
