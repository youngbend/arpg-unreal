// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGVitalSet.h"
#include "ARPGCore.h"
#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"

UARPGVitalSet::UARPGVitalSet()
{
	// Placeholder archetype values. Real numbers arrive with the ported
	// CombatStats assets in phase 4 -- these exist so a freshly spawned actor
	// is not sitting at zero health before anything initialises it.
	InitMaxHealth(100.f);
	InitHealth(100.f);
	InitMaxStamina(100.f);
	InitStamina(100.f);
	InitStaminaRegenRate(15.f);
	InitMaxMana(100.f);
	InitMana(100.f);
	InitManaRegenRate(1.f);
	InitMaxPoise(100.f);
	InitPoise(0.f); // accumulates upward -- see the header
}

void UARPGVitalSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// COND_None + REPNOTIFY_Always: every client needs these (health bars above
	// other players' heads), and we want the notify even when the value is
	// unchanged so aggregator rebasing stays correct.
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGVitalSet, Health,           COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGVitalSet, MaxHealth,        COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGVitalSet, Stamina,          COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGVitalSet, MaxStamina,       COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGVitalSet, StaminaRegenRate, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGVitalSet, Mana,             COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGVitalSet, MaxMana,          COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGVitalSet, ManaRegenRate,    COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGVitalSet, Poise,            COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UARPGVitalSet, MaxPoise,         COND_None, REPNOTIFY_Always);

	// Meta attributes are deliberately absent -- see the header.
}

void UARPGVitalSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	// This catches direct sets and max-value changes. It does NOT catch
	// effect-driven changes to current values -- those land in
	// PostGameplayEffectExecute below. Both are needed; clamping in only one
	// leaks out-of-range values through the other.
	if (Attribute == GetMaxHealthAttribute())
	{
		AdjustAttributeForMaxChange(Health, MaxHealth, NewValue, GetHealthAttribute());
	}
	else if (Attribute == GetMaxStaminaAttribute())
	{
		AdjustAttributeForMaxChange(Stamina, MaxStamina, NewValue, GetStaminaAttribute());
	}
	else if (Attribute == GetMaxManaAttribute())
	{
		AdjustAttributeForMaxChange(Mana, MaxMana, NewValue, GetManaAttribute());
	}
	else if (Attribute == GetHealthAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxHealth());
	}
	else if (Attribute == GetStaminaAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxStamina());
	}
	else if (Attribute == GetManaAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxMana());
	}
	else if (Attribute == GetPoiseAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxPoise());
	}
}

void UARPGVitalSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);

	// Damage. The execution has already resolved armor, resistance, penetration
	// and crit -- what arrives here is the final number.
	if (Data.EvaluatedData.Attribute == GetIncomingDamageAttribute())
	{
		const float LocalDamage = GetIncomingDamage();
		SetIncomingDamage(0.f); // consume immediately; never leave it set

		if (LocalDamage > 0.f)
		{
			SetHealth(FMath::Clamp(GetHealth() - LocalDamage, 0.f, GetMaxHealth()));
		}
	}
	// Healing. In Godot this was DamageType::CATEGORY_HEALING taking an early
	// branch through the same hitbox/hurtbox pipeline, so a heal can land on
	// anyone a hitbox touches. Same idea here, just a different meta attribute.
	else if (Data.EvaluatedData.Attribute == GetIncomingHealingAttribute())
	{
		const float LocalHealing = GetIncomingHealing();
		SetIncomingHealing(0.f);

		if (LocalHealing > 0.f)
		{
			SetHealth(FMath::Clamp(GetHealth() + LocalHealing, 0.f, GetMaxHealth()));
		}
	}
	// Poise is NOT accumulated here -- see OnPoiseDamageReceived in the header.
	// The size of this one contribution is what decides a flinch tier, so it is
	// handed on intact rather than folded into the running total first.
	else if (Data.EvaluatedData.Attribute == GetIncomingPoiseDamageAttribute())
	{
		const float LocalPoiseDamage = GetIncomingPoiseDamage();
		SetIncomingPoiseDamage(0.f);

		if (LocalPoiseDamage > 0.f)
		{
			OnPoiseDamageReceived.Broadcast(LocalPoiseDamage);
		}
	}
	// Non-meta attributes changed directly by an effect still need clamping --
	// PreAttributeChange does not see these.
	else if (Data.EvaluatedData.Attribute == GetHealthAttribute())
	{
		SetHealth(FMath::Clamp(GetHealth(), 0.f, GetMaxHealth()));
	}
	else if (Data.EvaluatedData.Attribute == GetStaminaAttribute())
	{
		SetStamina(FMath::Clamp(GetStamina(), 0.f, GetMaxStamina()));
	}
	else if (Data.EvaluatedData.Attribute == GetManaAttribute())
	{
		SetMana(FMath::Clamp(GetMana(), 0.f, GetMaxMana()));
	}
	else if (Data.EvaluatedData.Attribute == GetPoiseAttribute())
	{
		SetPoise(FMath::Clamp(GetPoise(), 0.f, GetMaxPoise()));
	}
}

void UARPGVitalSet::AdjustAttributeForMaxChange(
	const FGameplayAttributeData& AffectedAttribute,
	const FGameplayAttributeData& MaxAttribute,
	float NewMaxValue,
	const FGameplayAttribute& AffectedAttributeProperty) const
{
	UAbilitySystemComponent* ASC = GetOwningAbilitySystemComponent();
	const float CurrentMaxValue = MaxAttribute.GetCurrentValue();

	if (!FMath::IsNearlyEqual(CurrentMaxValue, NewMaxValue) && ASC)
	{
		const float CurrentValue = AffectedAttribute.GetCurrentValue();

		// Preserve the ratio rather than the absolute value: raising MaxHealth
		// from 100 to 200 on a character at 50 HP should leave them at 100, not
		// still at 50 (which reads as having been halved).
		const float NewDelta = (CurrentMaxValue > 0.f)
			? (CurrentValue * NewMaxValue / CurrentMaxValue) - CurrentValue
			: NewMaxValue;

		ASC->ApplyModToAttributeUnsafe(AffectedAttributeProperty, EGameplayModOp::Additive, NewDelta);
	}
}

void UARPGVitalSet::OnRep_Health(const FGameplayAttributeData& OldValue)           { ARPG_REPNOTIFY(UARPGVitalSet, Health, OldValue); }
void UARPGVitalSet::OnRep_MaxHealth(const FGameplayAttributeData& OldValue)        { ARPG_REPNOTIFY(UARPGVitalSet, MaxHealth, OldValue); }
void UARPGVitalSet::OnRep_Stamina(const FGameplayAttributeData& OldValue)          { ARPG_REPNOTIFY(UARPGVitalSet, Stamina, OldValue); }
void UARPGVitalSet::OnRep_MaxStamina(const FGameplayAttributeData& OldValue)       { ARPG_REPNOTIFY(UARPGVitalSet, MaxStamina, OldValue); }
void UARPGVitalSet::OnRep_StaminaRegenRate(const FGameplayAttributeData& OldValue) { ARPG_REPNOTIFY(UARPGVitalSet, StaminaRegenRate, OldValue); }
void UARPGVitalSet::OnRep_Mana(const FGameplayAttributeData& OldValue)             { ARPG_REPNOTIFY(UARPGVitalSet, Mana, OldValue); }
void UARPGVitalSet::OnRep_MaxMana(const FGameplayAttributeData& OldValue)          { ARPG_REPNOTIFY(UARPGVitalSet, MaxMana, OldValue); }
void UARPGVitalSet::OnRep_ManaRegenRate(const FGameplayAttributeData& OldValue)    { ARPG_REPNOTIFY(UARPGVitalSet, ManaRegenRate, OldValue); }
void UARPGVitalSet::OnRep_Poise(const FGameplayAttributeData& OldValue)            { ARPG_REPNOTIFY(UARPGVitalSet, Poise, OldValue); }
void UARPGVitalSet::OnRep_MaxPoise(const FGameplayAttributeData& OldValue)         { ARPG_REPNOTIFY(UARPGVitalSet, MaxPoise, OldValue); }
