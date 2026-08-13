// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGCombatLibrary.h"
#include "ARPGGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "GameFramework/Character.h"

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

void UARPGCombatLibrary::ApplyKnockback(AActor* Target, const FVector& Direction, float Force,
	bool bOverrideVertical)
{
	if (!Target || FMath::IsNearlyZero(Force) || Direction.IsNearlyZero())
	{
		return;
	}

	ACharacter* Character = Cast<ACharacter>(Target);
	if (!Character)
	{
		return; // see the header: not our problem to guess at
	}

	// Server only. LaunchCharacter writes velocity, which the movement component
	// already replicates; applying it on a client as well would fight that
	// correction rather than smooth it.
	if (!Character->HasAuthority())
	{
		return;
	}

	// A negative force reverses the line rather than needing its own direction:
	// out along the blow is a shove, back along it is a pull.
	Character->LaunchCharacter(Direction.GetSafeNormal() * Force,
		/*bXYOverride=*/true, /*bZOverride=*/bOverrideVertical);
}
