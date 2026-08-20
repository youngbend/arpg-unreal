// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGCombatDummy.h"
#include "ARPGAbilitySystemComponent.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGGameplayTags.h"
#include "ARPGHurtboxComponent.h"
#include "ARPGOffenseSet.h"
#include "ARPGResistanceSet.h"
#include "ARPGVitalSet.h"
#include "arpg.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"

AARPGCombatDummy::AARPGCombatDummy()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;

	Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Capsule"));
	Capsule->InitCapsuleSize(42.f, 96.f);
	// The Pawn profile puts this on the ECC_Pawn object channel, which is what
	// UARPGHitboxComponent sweeps for by default.
	Capsule->SetCollisionProfileName(TEXT("Pawn"));
	SetRootComponent(Capsule);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Capsule);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	Hurtbox = CreateDefaultSubobject<UARPGHurtboxComponent>(TEXT("Hurtbox"));

	// Minimal, not Mixed: nothing owns this actor, so no client needs the full
	// gameplay-effect list for it -- replicated tags and attributes are enough.
	Combatant.Create(*this, EGameplayEffectReplicationMode::Minimal);

	FactionTag = TAG_Faction_Enemy;
}

UAbilitySystemComponent* AARPGCombatDummy::GetAbilitySystemComponent() const
{
	return Combatant.AbilitySystem;
}

void AARPGCombatDummy::BeginPlay()
{
	Super::BeginPlay();

	if (!Combatant.AbilitySystem)
	{
		return;
	}

	// Owner and avatar are both this actor -- there is no PlayerState in the way.
	Combatant.AbilitySystem->InitAbilityActorInfo(this, this);

	// The health-change log is presentation, so it binds on every instance,
	// server and client alike. Watching it on the client is what proves the
	// attribute actually replicated rather than just changing on the server.
	if (bLogHealthChanges)
	{
		Combatant.AbilitySystem->GetGameplayAttributeValueChangeDelegate(
			UARPGVitalSet::GetHealthAttribute())
			.AddUObject(this, &AARPGCombatDummy::OnHealthChanged);
	}

	if (!HasAuthority())
	{
		return;
	}

	if (FactionTag.IsValid())
	{
		Combatant.AbilitySystem->AddLooseGameplayTag(
			FactionTag, 1, EGameplayTagReplicationState::TagOnly);
	}

	// SetNumericAttributeBase rather than applying an init GameplayEffect: this
	// is a test fixture, and a base-value set is the one case where bypassing
	// the effect pipeline is the honest thing to do.
	Combatant.AbilitySystem->SetNumericAttributeBase(UARPGVitalSet::GetMaxHealthAttribute(), MaxHealth);
	Combatant.AbilitySystem->SetNumericAttributeBase(UARPGVitalSet::GetHealthAttribute(), MaxHealth);
	Combatant.AbilitySystem->SetNumericAttributeBase(UARPGResistanceSet::GetBaseArmorAttribute(), BaseArmor);

	for (const TPair<TObjectPtr<UARPGDamageTypeAsset>, float>& Entry : Resistances)
	{
		const UARPGDamageTypeAsset* Type = Entry.Key;
		if (!Type)
		{
			continue;
		}

		if (!Type->ResistanceAttribute.IsValid())
		{
			UE_LOG(Logarpg, Warning,
				TEXT("%s: damage type '%s' has no ResistanceAttribute set, so its resistance "
				     "entry (%.2f) does nothing."),
				*GetName(), *GetNameSafe(Type), Entry.Value);
			continue;
		}

		Combatant.AbilitySystem->SetNumericAttributeBase(Type->ResistanceAttribute, Entry.Value);
	}
}

void AARPGCombatDummy::OnHealthChanged(const FOnAttributeChangeData& Data)
{
	const TCHAR* NetRole = HasAuthority() ? TEXT("SERVER") : TEXT("CLIENT");

	UE_LOG(Logarpg, Log,
		TEXT("[%s] %s health %.1f -> %.1f (delta %.1f)"),
		NetRole, *GetName(), Data.OldValue, Data.NewValue, Data.NewValue - Data.OldValue);
}
