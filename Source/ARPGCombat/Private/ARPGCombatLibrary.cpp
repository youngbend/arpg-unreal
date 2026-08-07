// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGCombatLibrary.h"
#include "ARPGGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"

bool UARPGCombatLibrary::TryResolveFaction(const AActor* Actor, FGameplayTag& OutFaction)
{
	OutFaction = FGameplayTag();
	if (!Actor)
	{
		return false;
	}

	const UAbilitySystemComponent* ASC =
		UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Actor);
	if (!ASC)
	{
		return false;
	}

	// Checked in a fixed order rather than by scanning for anything under
	// "Faction." so an actor that somehow owns two faction tags resolves
	// deterministically instead of by container iteration order.
	//
	// By value: FNativeGameplayTag::GetTag() returns an FGameplayTag by value, so
	// holding pointers into these would dangle. FGameplayTag is an FName wrapper,
	// so building the list per call costs nothing.
	const FGameplayTag Factions[] = {
		TAG_Faction_Player, TAG_Faction_Ally, TAG_Faction_Enemy, TAG_Faction_Neutral
	};

	for (const FGameplayTag& Faction : Factions)
	{
		if (ASC->HasMatchingGameplayTag(Faction))
		{
			OutFaction = Faction;
			return true;
		}
	}

	return false;
}

bool UARPGCombatLibrary::AreFactionsHostile(FGameplayTag FactionA, FGameplayTag FactionB)
{
	if (FactionA == TAG_Faction_Neutral || FactionB == TAG_Faction_Neutral)
	{
		return false;
	}

	const bool bAGood = (FactionA == TAG_Faction_Player || FactionA == TAG_Faction_Ally);
	const bool bBGood = (FactionB == TAG_Faction_Player || FactionB == TAG_Faction_Ally);

	return bAGood != bBGood;
}

bool UARPGCombatLibrary::CanDamage(const AActor* SourceActor, const AActor* TargetActor)
{
	FGameplayTag SourceFaction;
	FGameplayTag TargetFaction;

	// Either side unfactioned => no filtering, the hit lands. See TryResolveFaction.
	if (!TryResolveFaction(SourceActor, SourceFaction)) { return true; }
	if (!TryResolveFaction(TargetActor, TargetFaction)) { return true; }

	return AreFactionsHostile(SourceFaction, TargetFaction);
}
