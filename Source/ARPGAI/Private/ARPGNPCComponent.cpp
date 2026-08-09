// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGNPCComponent.h"
#include "ARPGAI.h"
#include "ARPGGameplayTags.h"
#include "ARPGHurtboxComponent.h"
#include "ARPGNPCDefinition.h"
#include "ARPGPerceptionComponent.h"
#include "ARPGVitalSet.h"
#include "ARPGWeaponComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "GameFramework/Pawn.h"

UARPGNPCComponent::UARPGNPCComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UARPGNPCComponent::BeginPlay()
{
	Super::BeginPlay();

	if (GetOwner() && GetOwner()->HasAuthority())
	{
		ApplyDefinition();
	}

	// Bound on every machine: the hurtbox event is where a hit becomes knowledge,
	// and the handler itself is server-gated.
	if (UARPGHurtboxComponent* Hurtbox =
			GetOwner() ? GetOwner()->FindComponentByClass<UARPGHurtboxComponent>() : nullptr)
	{
		Hurtbox->OnHitReceived.AddDynamic(this, &UARPGNPCComponent::HandleHitReceived);
	}
}

void UARPGNPCComponent::ApplyDefinition()
{
	if (!Definition)
	{
		UE_LOG(LogARPGAI, Warning,
			TEXT("%s has an NPC component with no definition, so it has no archetype: no stats, "
			     "no weapon and no behaviour tree."),
			*GetNameSafe(GetOwner()));
		return;
	}

	UAbilitySystemComponent* ASC =
		UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetOwner());

	if (ASC)
	{
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxHealthAttribute(), Definition->MaxHealth);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetHealthAttribute(), Definition->MaxHealth);

		// Poise is an ATTRIBUTE, not a field on the poise component -- the
		// component owns the meter's behaviour, the attribute owns its ceiling.
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxPoiseAttribute(), Definition->MaxPoise);

		if (Definition->FactionTag.IsValid())
		{
			ASC->AddLooseGameplayTag(Definition->FactionTag, 1, EGameplayTagReplicationState::TagOnly);
		}
	}



	// Through the weapon component rather than written onto the hitbox directly,
	// so an NPC's weapon brings its moveset exactly as a player's does.
	if (Definition->Weapon)
	{
		if (UARPGWeaponComponent* Weapon = GetOwner()->FindComponentByClass<UARPGWeaponComponent>())
		{
			Weapon->EquipWeapon(Definition->Weapon);
		}
	}
}

void UARPGNPCComponent::HandleHitReceived(const FGameplayEffectContextHandle& Context, float Magnitude)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	AActor* Attacker = Context.GetInstigator();
	if (!Attacker)
	{
		return;
	}

	// Perception lives on the CONTROLLER, so an unpossessed NPC simply does not
	// react -- which is the right answer for a body with nobody driving it.
	const APawn* Pawn = Cast<APawn>(GetOwner());
	AController* Controller = Pawn ? Pawn->GetController() : nullptr;
	UARPGPerceptionComponent* Perception =
		Controller ? Controller->FindComponentByClass<UARPGPerceptionComponent>() : nullptr;

	if (!Perception)
	{
		return;
	}

	// The attacker's position, not the impact point: the NPC is working out where
	// the blow came FROM so it knows where to go and look.
	Perception->NotifyDamagedBy(Attacker, Attacker->GetActorLocation());
}
