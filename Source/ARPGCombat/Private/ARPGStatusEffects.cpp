// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGStatusEffects.h"
#include "ARPGDamageExecution.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGGameplayTags.h"
#include "ARPGOffenseSet.h"
#include "ARPGResistanceSet.h"
#include "ARPGStatusApplicationComponent.h"
#include "ARPGStatusEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/RemoveOtherGameplayEffectComponent.h"

namespace
{
	/** Adds a periodic damage tick using the shared damage pipeline. */
	void AddDamageTick(UGameplayEffect& Effect, float Period)
	{
		Effect.Period = Period;

		FGameplayEffectExecutionDefinition ExecutionDef;
		ExecutionDef.CalculationClass = UARPGDamageExecution::StaticClass();
		Effect.Executions.Add(ExecutionDef);
	}

	/** Additive modifier on a target attribute, e.g. Wet's fire resistance. */
	void AddAdditiveModifier(UGameplayEffect& Effect, const FGameplayAttribute& Attribute, float Value)
	{
		FGameplayModifierInfo Mod;
		Mod.Attribute = Attribute;
		Mod.ModifierOp = EGameplayModOp::Additive;
		Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Value));
		Effect.Modifiers.Add(Mod);
	}

	/**
	 * MultiplyCompound, NOT the older Multiplicitive spelling.
	 *
	 * They are different operations and the old name is misleading:
	 * EGameplayModOp::Multiplicitive is a back-compat alias for MultiplyAdditive,
	 * which SUMS the multipliers' deltas from 1 before applying them once --
	 * two 0.5x debuffs give (0.5-1)+(0.5-1)+1 = 0.0, wiping outgoing damage
	 * entirely rather than reducing it to 0.25x. MultiplyCompound multiplies them
	 * together, which is what add_damage_amp() did in Godot and what
	 * UARPGOffenseSet's header describes.
	 *
	 * The damage execution treats a zero amp as "no modifier", so under the old
	 * op a second concurrent debuff silently removed the first one's effect.
	 */
	void AddMultiplyModifier(UGameplayEffect& Effect, const FGameplayAttribute& Attribute, float Value)
	{
		FGameplayModifierInfo Mod;
		Mod.Attribute = Attribute;
		Mod.ModifierOp = EGameplayModOp::MultiplyCompound;
		Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Value));
		Effect.Modifiers.Add(Mod);
	}
}

// ---------------------------------------------------------------------------

UARPGStatusGameplayEffect::UARPGStatusGameplayEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;

	// Aggregate by target, not by source: two different attackers setting the
	// same character alight should build one shared fire, matching Godot's
	// single active_effects entry keyed by effect id. AggregateBySource would
	// give each attacker a private stack and multiply the damage by the number
	// of people involved.
	//
	// The field is deprecated as of 5.7 in favour of GetStackingType, but the
	// matching SetStackingType is WITH_EDITOR-only -- so a runtime constructor
	// has no other way to declare this yet. Suppressed deliberately rather than
	// left to warn, and worth revisiting when Epic ships a runtime setter.
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	StackingType = EGameplayEffectStackingType::AggregateByTarget;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	StackDurationRefreshPolicy = EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication;

	// Restart the tick clock when a stack lands, matching
	// StatusEffectInstance::add_stacks resetting tick_timer.
	StackPeriodResetPolicy = EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication;

	// The alive gate and target resistance apply to every status.
	AddDefaultComponent<UARPGStatusApplicationComponent>(TEXT("StatusApplication"));
}

// ---------------------------------------------------------------------------

UARPGStatusEffect_Burning::UARPGStatusEffect_Burning()
{
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(5.f));
	StackLimitCount = 5;

	UARPGStatusEffectComponent& Status = AddDefaultComponent<UARPGStatusEffectComponent>(TEXT("StatusInfo"));
	Status.StatusTag = TAG_Status_Burning;
	Status.DisplayName = NSLOCTEXT("ARPGStatus", "Burning", "Burning");
	Status.bIsDebuff = true;
	Status.VfxFit = EARPGStatusVfxFit::Bounds;
	Status.VfxPriority = 10;
	Status.TickDamagePerStack = 5.f;
	Status.TickDamageType = TSoftObjectPtr<UARPGDamageTypeAsset>(
		FSoftObjectPath(TEXT("/Game/ARPG/DamageTypes/DA_Damage_Fire.DA_Damage_Fire")));

	AddDamageTick(*this, /*Period=*/1.f);
	{
		FInheritedTagContainer Granted;
		Granted.Added.AddTag(TAG_Status_Burning);
		AddDefaultComponent<UTargetTagsGameplayEffectComponent>(TEXT("TargetTags"))
			.SetAndApplyTargetTagChanges(Granted);
	}
}

// ---------------------------------------------------------------------------

UARPGStatusEffect_Shocked::UARPGStatusEffect_Shocked()
{
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(3.f));
	StackLimitCount = 3;

	UARPGStatusEffectComponent& Status = AddDefaultComponent<UARPGStatusEffectComponent>(TEXT("StatusInfo"));
	Status.StatusTag = TAG_Status_Shocked;
	Status.DisplayName = NSLOCTEXT("ARPGStatus", "Shocked", "Shocked");
	Status.bIsDebuff = true;
	Status.VfxFit = EARPGStatusVfxFit::Bounds;
	Status.VfxPriority = 20;
	Status.TickDamagePerStack = 2.f;
	Status.TickDamageType = TSoftObjectPtr<UARPGDamageTypeAsset>(
		FSoftObjectPath(TEXT("/Game/ARPG/DamageTypes/DA_Damage_Lightning.DA_Damage_Lightning")));

	AddDamageTick(*this, /*Period=*/0.5f);
	{
		FInheritedTagContainer Granted;
		Granted.Added.AddTag(TAG_Status_Shocked);
		AddDefaultComponent<UTargetTagsGameplayEffectComponent>(TEXT("TargetTags"))
			.SetAndApplyTargetTagChanges(Granted);
	}
}

// ---------------------------------------------------------------------------

UARPGStatusEffect_Wet::UARPGStatusEffect_Wet()
{
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(6.f));
	StackLimitCount = 3;

	UARPGStatusEffectComponent& Status = AddDefaultComponent<UARPGStatusEffectComponent>(TEXT("StatusInfo"));
	Status.StatusTag = TAG_Status_Wet;
	Status.DisplayName = NSLOCTEXT("ARPGStatus", "Wet", "Wet");
	Status.bIsDebuff = false; // being soaked is protective here
	Status.VfxFit = EARPGStatusVfxFit::Bounds;
	Status.VfxPriority = 5;

	// The whole of wet.gd, as one line. GAS scales this by stack count and
	// re-evaluates on every stack change, which is what the script's meta-flag
	// and _on_stack_changed hook existed to hand-roll.
	AddAdditiveModifier(*this, UARPGResistanceSet::GetFireResistanceAttribute(),
		FireResistancePerStack);

	// Wet and Burning cannot coexist -- target.remove_status_effect("burning"),
	// natively.
	URemoveOtherGameplayEffectComponent& Remove =
		AddDefaultComponent<URemoveOtherGameplayEffectComponent>(TEXT("RemoveBurning"));
	FGameplayEffectQuery Query;
	Query.OwningTagQuery = FGameplayTagQuery::MakeQuery_MatchAnyTags(
		FGameplayTagContainer(TAG_Status_Burning));
	Remove.RemoveGameplayEffectQueries.Add(Query);

	{
		FInheritedTagContainer Granted;
		Granted.Added.AddTag(TAG_Status_Wet);
		AddDefaultComponent<UTargetTagsGameplayEffectComponent>(TEXT("TargetTags"))
			.SetAndApplyTargetTagChanges(Granted);
	}
}

// ---------------------------------------------------------------------------

UARPGStatusEffect_Weakened::UARPGStatusEffect_Weakened()
{
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(8.f));
	StackLimitCount = 1;

	UARPGStatusEffectComponent& Status = AddDefaultComponent<UARPGStatusEffectComponent>(TEXT("StatusInfo"));
	Status.StatusTag = TAG_Status_Weakened;
	Status.DisplayName = NSLOCTEXT("ARPGStatus", "Weakened", "Weakened");
	Status.bIsDebuff = true;
	Status.VfxFit = EARPGStatusVfxFit::Bounds;
	Status.VfxPriority = 1;

	// The whole of weakened.gd. Multiply rather than Additive so concurrent
	// amps and debuffs compose the way add_damage_amp's multiplicative stacking
	// did.
	AddMultiplyModifier(*this, UARPGOffenseSet::GetDamageAmpMultiplierAttribute(),
		DamageAmpMultiplier);

	{
		FInheritedTagContainer Granted;
		Granted.Added.AddTag(TAG_Status_Weakened);
		AddDefaultComponent<UTargetTagsGameplayEffectComponent>(TEXT("TargetTags"))
			.SetAndApplyTargetTagChanges(Granted);
	}
}
