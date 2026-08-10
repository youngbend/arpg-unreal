// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGEquipmentEffect.h"
#include "ARPGCombat.h"
#include "ARPGGameplayTags.h"
#include "ARPGOffenseSet.h"
#include "ARPGResistanceSet.h"
#include "AbilitySystemComponent.h"

namespace
{
	/**
	 * Every attribute a worn piece may move, paired with the SetByCaller that
	 * drives it.
	 *
	 * Built once. FNativeGameplayTag::GetTag() returns by value, so this holds
	 * resolved FGameplayTags rather than pointers into the native tag objects.
	 */
	struct FARPGEquipmentModifiers
	{
		TArray<TPair<FGameplayAttribute, FGameplayTag>> Entries;

		FARPGEquipmentModifiers()
		{
			Entries.Emplace(UARPGResistanceSet::GetBaseArmorAttribute(),
				TAG_Data_Equipment_Armor.GetTag());
			Entries.Emplace(UARPGOffenseSet::GetCritChanceAttribute(),
				TAG_Data_Equipment_CritChance.GetTag());
			Entries.Emplace(UARPGResistanceSet::GetPhysicalResistanceAttribute(),
				TAG_Data_Equipment_Resistance_Physical.GetTag());
			Entries.Emplace(UARPGResistanceSet::GetFireResistanceAttribute(),
				TAG_Data_Equipment_Resistance_Fire.GetTag());
			Entries.Emplace(UARPGResistanceSet::GetIceResistanceAttribute(),
				TAG_Data_Equipment_Resistance_Ice.GetTag());
			Entries.Emplace(UARPGResistanceSet::GetLightResistanceAttribute(),
				TAG_Data_Equipment_Resistance_Light.GetTag());
			Entries.Emplace(UARPGResistanceSet::GetLightningResistanceAttribute(),
				TAG_Data_Equipment_Resistance_Lightning.GetTag());
			Entries.Emplace(UARPGResistanceSet::GetWaterResistanceAttribute(),
				TAG_Data_Equipment_Resistance_Water.GetTag());
			Entries.Emplace(UARPGResistanceSet::GetFallResistanceAttribute(),
				TAG_Data_Equipment_Resistance_Fall.GetTag());
		}
	};

	const FARPGEquipmentModifiers& EquipmentModifiers()
	{
		static FARPGEquipmentModifiers Modifiers;
		return Modifiers;
	}
}

UARPGEquipmentGameplayEffect::UARPGEquipmentGameplayEffect()
{
	// Infinite: the grant lasts exactly as long as the piece is worn, and the
	// component removes it by handle. Nothing here expires on its own.
	DurationPolicy = EGameplayEffectDurationType::Infinite;

	for (const TPair<FGameplayAttribute, FGameplayTag>& Entry : EquipmentModifiers().Entries)
	{
		// FSetByCallerFloat has only a default constructor, so the tag is assigned
		// rather than passed.
		FSetByCallerFloat SetByCaller;
		SetByCaller.DataTag = Entry.Value;

		FGameplayModifierInfo& Mod = Modifiers.AddDefaulted_GetRef();
		Mod.Attribute = Entry.Key;
		Mod.ModifierOp = EGameplayModOp::Additive;
		Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(SetByCaller);
	}
}

// ---------------------------------------------------------------------------

FGameplayTag UARPGEquipmentEffectLibrary::FindSetByCallerTag(const FGameplayAttribute& Attribute)
{
	for (const TPair<FGameplayAttribute, FGameplayTag>& Entry : EquipmentModifiers().Entries)
	{
		if (Entry.Key == Attribute)
		{
			return Entry.Value;
		}
	}
	return FGameplayTag();
}

void UARPGEquipmentEffectLibrary::RemoveGrant(UAbilitySystemComponent* ASC,
	FActiveGameplayEffectHandle& InOutHandle)
{
	if (ASC && InOutHandle.IsValid())
	{
		ASC->RemoveActiveGameplayEffect(InOutHandle);
	}
	InOutHandle.Invalidate();
}

void UARPGEquipmentEffectLibrary::ApplyGrant(UAbilitySystemComponent* ASC,
	const TArray<TPair<FGameplayAttribute, float>>& Grants,
	FActiveGameplayEffectHandle& InOutHandle)
{
	// Removed first and unconditionally, so swapping a piece can never leave the
	// previous one's contribution behind.
	RemoveGrant(ASC, InOutHandle);

	if (!ASC || Grants.Num() == 0)
	{
		return;
	}

	const FGameplayEffectSpecHandle SpecHandle = ASC->MakeOutgoingSpec(
		UARPGEquipmentGameplayEffect::StaticClass(), 1.f, ASC->MakeEffectContext());

	if (!SpecHandle.IsValid())
	{
		return;
	}

	// Every modifier is set, including the ones this piece does not use: an
	// unset SetByCaller warns on every evaluation and evaluates to zero anyway,
	// so writing the zeros explicitly is both cheaper and quieter.
	for (const TPair<FGameplayAttribute, FGameplayTag>& Entry : EquipmentModifiers().Entries)
	{
		SpecHandle.Data->SetSetByCallerMagnitude(Entry.Value, 0.f);
	}

	bool bAnyApplied = false;

	for (const TPair<FGameplayAttribute, float>& Grant : Grants)
	{
		if (Grant.Value == 0.f)
		{
			continue;
		}

		const FGameplayTag Tag = FindSetByCallerTag(Grant.Key);
		if (!Tag.IsValid())
		{
			// The caller named an attribute the equipment effect has no modifier
			// for. Loud, because the alternative is a piece whose stat line reads
			// correctly in the UI and does nothing in the fight.
			UE_LOG(LogARPGCombat, Warning,
				TEXT("Equipment grants %.2f to '%s', which UARPGEquipmentGameplayEffect carries no "
				     "modifier for. The bonus will not apply. Add it to FARPGEquipmentModifiers."),
				Grant.Value, *Grant.Key.GetName());
			continue;
		}

		SpecHandle.Data->SetSetByCallerMagnitude(Tag, Grant.Value);
		bAnyApplied = true;
	}

	if (bAnyApplied)
	{
		InOutHandle = ASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data);
	}
}
