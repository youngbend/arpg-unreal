// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGArmorDefinition.h"
#include "ARPGGameplayTags.h"
#include "ARPGDamageTypeAsset.h"

float UARPGArmorDefinition::GetTotalArmorValue() const
{
	float Total = ArmorValue;
	for (const FARPGArmorModifier& Modifier : Modifiers)
	{
		Total += Modifier.FlatArmorBonus;
	}
	return FMath::Max(0.f, Total);
}

float UARPGArmorDefinition::GetTotalResistanceBonus(const UARPGDamageTypeAsset* DamageType) const
{
	if (!DamageType)
	{
		return 0.f;
	}

	float Total = 0.f;
	for (const FARPGArmorModifier& Modifier : Modifiers)
	{
		if (const float* Found = Modifier.ResistanceBonuses.Find(const_cast<UARPGDamageTypeAsset*>(DamageType)))
		{
			Total += *Found;
		}
	}
	return Total;
}

TMap<TObjectPtr<UARPGDamageTypeAsset>, float> UARPGArmorDefinition::GetAggregatedResistances() const
{
	TMap<TObjectPtr<UARPGDamageTypeAsset>, float> Aggregated;
	for (const FARPGArmorModifier& Modifier : Modifiers)
	{
		for (const TPair<TObjectPtr<UARPGDamageTypeAsset>, float>& Entry : Modifier.ResistanceBonuses)
		{
			if (Entry.Key)
			{
				Aggregated.FindOrAdd(Entry.Key) += Entry.Value;
			}
		}
	}
	return Aggregated;
}

FName UARPGArmorDefinition::ArmorTypeToName(EARPGArmorType Type)
{
	switch (Type)
	{
	case EARPGArmorType::Light:  return TEXT("light");
	case EARPGArmorType::Medium: return TEXT("medium");
	case EARPGArmorType::Heavy:  return TEXT("heavy");
	case EARPGArmorType::Unarmored:
	default:                     return TEXT("unarmored");
	}
}

FGameplayTag UARPGArmorDefinition::ArmorTypeToTag(EARPGArmorType Type)
{
	switch (Type)
	{
	case EARPGArmorType::Light:  return TAG_Armor_Light;
	case EARPGArmorType::Medium: return TAG_Armor_Medium;
	case EARPGArmorType::Heavy:  return TAG_Armor_Heavy;
	case EARPGArmorType::Unarmored:
	default:                     return TAG_Armor_Unarmored;
	}
}
