// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGWeaponDefinition.h"
#include "ARPGDamageTypeAsset.h"

float UARPGWeaponDefinition::GetTotalFlatDamageBonus() const
{
	float Total = 0.f;
	for (const FARPGWeaponModifier& Modifier : Modifiers)
	{
		Total += Modifier.FlatDamageBonus;
	}
	return Total;
}

float UARPGWeaponDefinition::GetTotalCritChanceBonus() const
{
	float Total = 0.f;
	for (const FARPGWeaponModifier& Modifier : Modifiers)
	{
		Total += Modifier.CritChanceBonus;
	}
	return FMath::Clamp(Total, 0.f, 1.f);
}

float UARPGWeaponDefinition::GetTotalTypeMultiplier(FGameplayTag DamageTypeTag) const
{
	if (!DamageTypeTag.IsValid())
	{
		return 1.f;
	}

	// Additive across modifiers, starting from 1: two +20% affixes give 1.4, not
	// 1.44. Multiplicative stacking of rolled affixes runs away far too quickly
	// once a weapon carries several.
	float Total = 1.f;
	for (const FARPGWeaponModifier& Modifier : Modifiers)
	{
		if (const float* Found = Modifier.TypeMultipliers.Find(DamageTypeTag))
		{
			Total += (*Found - 1.f);
		}
	}
	return FMath::Max(0.f, Total);
}

float UARPGWeaponDefinition::GetEffectiveDamage(float MotionValue) const
{
	const float Raw = (BaseDamage + GetTotalFlatDamageBonus()) * FMath::Max(0.f, MotionValue);
	const FGameplayTag TypeTag = BaseDamageType ? BaseDamageType->DamageTypeTag : FGameplayTag();
	return Raw * GetTotalTypeMultiplier(TypeTag);
}

FName UARPGWeaponDefinition::WeaponTypeToName(EARPGWeaponType Type)
{
	switch (Type)
	{
	case EARPGWeaponType::Sword:      return TEXT("sword");
	case EARPGWeaponType::Axe:        return TEXT("axe");
	case EARPGWeaponType::Mace:       return TEXT("mace");
	case EARPGWeaponType::Spear:      return TEXT("spear");
	case EARPGWeaponType::Dagger:     return TEXT("dagger");
	case EARPGWeaponType::Greatsword: return TEXT("greatsword");
	case EARPGWeaponType::Staff:      return TEXT("staff");
	case EARPGWeaponType::Wand:       return TEXT("wand");
	case EARPGWeaponType::Orb:        return TEXT("orb");
	case EARPGWeaponType::Unarmed:
	default:                          return TEXT("unarmed");
	}
}
