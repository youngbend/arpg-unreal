// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGArmorComponent.h"
#include "ARPGArmorDefinition.h"
#include "ARPGCombat.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGEquipmentEffect.h"
#include "ARPGResistanceSet.h"
#include "AbilitySystemComponent.h"
#include "Net/UnrealNetwork.h"

UARPGArmorComponent::UARPGArmorComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UARPGArmorComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UARPGArmorComponent, Armor);
}

void UARPGArmorComponent::BeginPlay()
{
	Super::BeginPlay();

	// Attributes are server-authoritative, so only the server applies the
	// contribution; clients receive the resulting values by replication.
	if (HasAuthority() && DefaultArmor && !Armor)
	{
		EquipArmor(DefaultArmor);
	}
}

FName UARPGArmorComponent::GetArmorTypeName() const
{
	return Armor ? UARPGArmorDefinition::ArmorTypeToName(Armor->ArmorType)
	             : UARPGArmorDefinition::ArmorTypeToName(EARPGArmorType::Unarmored);
}

void UARPGArmorComponent::EquipArmor(UARPGArmorDefinition* NewArmor)
{
	if (Armor == NewArmor)
	{
		return;
	}

	Armor = NewArmor;
	RefreshAttributes();
	OnArmorChanged.Broadcast(Armor);

	UE_LOG(LogARPGCombat, Log, TEXT("%s equipped armour '%s' (+%.0f armour)"),
		*GetNameSafe(GetOwner()),
		Armor ? *Armor->ArmorId.ToString() : TEXT("<none>"),
		Armor ? Armor->GetTotalArmorValue() : 0.f);
}

void UARPGArmorComponent::UnequipArmor()
{
	EquipArmor(nullptr);
}

void UARPGArmorComponent::RefreshAttributes()
{
	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC || !HasAuthority())
	{
		return;
	}

	// Nothing worn: drop the grant and stop. ApplyGrant would do the same with an
	// empty list, but saying so here keeps the unequip path obvious.
	if (!Armor)
	{
		UARPGEquipmentEffectLibrary::RemoveGrant(ASC, GrantHandle);
		return;
	}

	TArray<TPair<FGameplayAttribute, float>> Grants;

	const float ArmorTotal = Armor->GetTotalArmorValue();
	if (ArmorTotal != 0.f)
	{
		Grants.Emplace(UARPGResistanceSet::GetBaseArmorAttribute(), ArmorTotal);
	}

	for (const TPair<TObjectPtr<UARPGDamageTypeAsset>, float>& Entry : Armor->GetAggregatedResistances())
	{
		const UARPGDamageTypeAsset* DamageType = Entry.Key;
		if (!DamageType || Entry.Value == 0.f)
		{
			continue;
		}

		if (!DamageType->ResistanceAttribute.IsValid())
		{
			// The asset names no attribute, so there is nothing to raise -- and
			// silently ignoring it would make the armour look like it simply
			// does not work against that type.
			UE_LOG(LogARPGCombat, Warning,
				TEXT("Armour '%s' grants %.2f resistance to '%s', but that damage type has no "
				     "ResistanceAttribute set, so the bonus does nothing."),
				*Armor->ArmorId.ToString(), Entry.Value, *GetNameSafe(DamageType));
			continue;
		}

		Grants.Emplace(DamageType->ResistanceAttribute, Entry.Value);
	}

	// One call replaces the whole previous remove-then-reapply dance: the old
	// effect is removed by handle, which reverses every modifier it carried
	// exactly, whatever else has touched those attributes since.
	UARPGEquipmentEffectLibrary::ApplyGrant(ASC, Grants, GrantHandle);
}

void UARPGArmorComponent::OnRep_Armor()
{
	OnArmorChanged.Broadcast(Armor);
}
