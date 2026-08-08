// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGArmorComponent.h"
#include "ARPGArmorDefinition.h"
#include "ARPGCombat.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGResistanceSet.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Engine/AssetManager.h"
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
	if (GetOwner() && GetOwner()->HasAuthority() && DefaultArmor && !Armor)
	{
		EquipArmor(DefaultArmor);
	}
}

UAbilitySystemComponent* UARPGArmorComponent::GetASC() const
{
	if (!CachedASC)
	{
		CachedASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetOwner());
	}
	return CachedASC;
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
	if (!ASC || !GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	// Reverse exactly what was applied, rather than recomputing it from the
	// definition -- see the header. A modifier rerolled while the piece is worn
	// would otherwise leave a permanent drift in the wearer's defences.
	if (AppliedArmorValue != 0.f)
	{
		const float Current = ASC->GetNumericAttribute(UARPGResistanceSet::GetBaseArmorAttribute());
		ASC->SetNumericAttributeBase(UARPGResistanceSet::GetBaseArmorAttribute(),
			FMath::Max(0.f, Current - AppliedArmorValue));
		AppliedArmorValue = 0.f;
	}

	for (const TPair<FGameplayAttribute, float>& Applied : AppliedResistances)
	{
		const float Current = ASC->GetNumericAttribute(Applied.Key);
		ASC->SetNumericAttributeBase(Applied.Key, Current - Applied.Value);
	}
	AppliedResistances.Reset();

	if (!Armor)
	{
		return;
	}

	const float ArmorTotal = Armor->GetTotalArmorValue();
	if (ArmorTotal != 0.f)
	{
		const float Current = ASC->GetNumericAttribute(UARPGResistanceSet::GetBaseArmorAttribute());
		ASC->SetNumericAttributeBase(UARPGResistanceSet::GetBaseArmorAttribute(), Current + ArmorTotal);
		AppliedArmorValue = ArmorTotal;
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

		const float Current = ASC->GetNumericAttribute(DamageType->ResistanceAttribute);
		ASC->SetNumericAttributeBase(DamageType->ResistanceAttribute, Current + Entry.Value);
		AppliedResistances.Emplace(DamageType->ResistanceAttribute, Entry.Value);
	}
}

void UARPGArmorComponent::OnRep_Armor()
{
	OnArmorChanged.Broadcast(Armor);
}
