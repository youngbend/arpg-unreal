// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGDamageExecution.h"
#include "ARPGCombat.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGGameplayEffectContext.h"
#include "ARPGGameplayTags.h"
#include "ARPGOffenseSet.h"
#include "ARPGResistanceSet.h"
#include "ARPGVitalSet.h"

namespace
{
	/**
	 * Capture definitions must be declared statically, so every resistance
	 * attribute is captured whether or not a given hit uses it. The map is what
	 * turns that static set back into a data-driven lookup: the damage type
	 * asset names an FGameplayAttribute, and this resolves it to the capture.
	 *
	 * ADDING A DAMAGE TYPE: add the attribute to UARPGResistanceSet, add a
	 * DECLARE/DEFINE pair here, add it to ResistanceCaptures, and add it to
	 * RelevantAttributesToCapture in the constructor. Then the new type is
	 * authored entirely as an asset.
	 */
	struct FARPGDamageStatics
	{
		DECLARE_ATTRIBUTE_CAPTUREDEF(BaseArmor);
		DECLARE_ATTRIBUTE_CAPTUREDEF(PhysicalResistance);
		DECLARE_ATTRIBUTE_CAPTUREDEF(FireResistance);
		DECLARE_ATTRIBUTE_CAPTUREDEF(IceResistance);
		DECLARE_ATTRIBUTE_CAPTUREDEF(LightResistance);
		DECLARE_ATTRIBUTE_CAPTUREDEF(LightningResistance);
		DECLARE_ATTRIBUTE_CAPTUREDEF(WaterResistance);
		DECLARE_ATTRIBUTE_CAPTUREDEF(FallResistance);

		DECLARE_ATTRIBUTE_CAPTUREDEF(AttackPower);
		DECLARE_ATTRIBUTE_CAPTUREDEF(CritMultiplier);
		DECLARE_ATTRIBUTE_CAPTUREDEF(DamageAmpMultiplier);

		/** FGameplayAttribute -> its capture definition, for the tag-driven lookup. */
		TMap<FGameplayAttribute, FGameplayEffectAttributeCaptureDefinition> ResistanceCaptures;

		FARPGDamageStatics()
		{
			// Target-side. Snapshot=false: read at execution time, so a resistance
			// buff applied between the swing starting and the hit landing counts.
			DEFINE_ATTRIBUTE_CAPTUREDEF(UARPGResistanceSet, BaseArmor,           Target, false);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UARPGResistanceSet, PhysicalResistance,  Target, false);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UARPGResistanceSet, FireResistance,      Target, false);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UARPGResistanceSet, IceResistance,       Target, false);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UARPGResistanceSet, LightResistance,     Target, false);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UARPGResistanceSet, LightningResistance, Target, false);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UARPGResistanceSet, WaterResistance,     Target, false);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UARPGResistanceSet, FallResistance,      Target, false);

			// Source-side.
			DEFINE_ATTRIBUTE_CAPTUREDEF(UARPGOffenseSet, AttackPower,         Source, false);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UARPGOffenseSet, CritMultiplier,      Source, false);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UARPGOffenseSet, DamageAmpMultiplier, Source, false);

			ResistanceCaptures.Add(UARPGResistanceSet::GetPhysicalResistanceAttribute(),  PhysicalResistanceDef);
			ResistanceCaptures.Add(UARPGResistanceSet::GetFireResistanceAttribute(),      FireResistanceDef);
			ResistanceCaptures.Add(UARPGResistanceSet::GetIceResistanceAttribute(),       IceResistanceDef);
			ResistanceCaptures.Add(UARPGResistanceSet::GetLightResistanceAttribute(),     LightResistanceDef);
			ResistanceCaptures.Add(UARPGResistanceSet::GetLightningResistanceAttribute(), LightningResistanceDef);
			ResistanceCaptures.Add(UARPGResistanceSet::GetWaterResistanceAttribute(),     WaterResistanceDef);
			ResistanceCaptures.Add(UARPGResistanceSet::GetFallResistanceAttribute(),      FallResistanceDef);
		}
	};

	const FARPGDamageStatics& DamageStatics()
	{
		static FARPGDamageStatics Statics;
		return Statics;
	}
}

UARPGDamageExecution::UARPGDamageExecution()
{
	const FARPGDamageStatics& S = DamageStatics();

	RelevantAttributesToCapture.Add(S.BaseArmorDef);
	RelevantAttributesToCapture.Add(S.PhysicalResistanceDef);
	RelevantAttributesToCapture.Add(S.FireResistanceDef);
	RelevantAttributesToCapture.Add(S.IceResistanceDef);
	RelevantAttributesToCapture.Add(S.LightResistanceDef);
	RelevantAttributesToCapture.Add(S.LightningResistanceDef);
	RelevantAttributesToCapture.Add(S.WaterResistanceDef);
	RelevantAttributesToCapture.Add(S.FallResistanceDef);

	RelevantAttributesToCapture.Add(S.AttackPowerDef);
	RelevantAttributesToCapture.Add(S.CritMultiplierDef);
	RelevantAttributesToCapture.Add(S.DamageAmpMultiplierDef);
}

void UARPGDamageExecution::Execute_Implementation(
	const FGameplayEffectCustomExecutionParameters& ExecutionParams,
	FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
	const FARPGDamageStatics& S = DamageStatics();
	const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();

	FAggregatorEvaluateParameters EvalParams;
	EvalParams.SourceTags = Spec.CapturedSourceTags.GetAggregatedTags();
	EvalParams.TargetTags = Spec.CapturedTargetTags.GetAggregatedTags();

	const FARPGGameplayEffectContext* Context =
		FARPGGameplayEffectContext::ExtractFrom(Spec.GetContext());

	const UARPGDamageTypeAsset* DamageType =
		Context ? Context->DamageType.Get() : nullptr;

	// --- 2. Raw ---------------------------------------------------------------
	// SetByCaller rather than a fixed magnitude: the number comes from the
	// weapon/attack/spell at swing time, exactly as base_amount did in Godot.
	const float BaseDamage = Spec.GetSetByCallerMagnitude(TAG_Data_Damage, /*WarnIfNotFound=*/false, 0.f);

	float AttackPower = 0.f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(S.AttackPowerDef, EvalParams, AttackPower);

	float DamageAmp = 1.f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(S.DamageAmpMultiplierDef, EvalParams, DamageAmp);
	if (DamageAmp <= 0.f)
	{
		// A zero/negative amp would invert or erase the hit. Treat it as "no
		// modifier" -- a debuff that should null damage entirely belongs as an
		// immunity, not as a silent multiply-by-zero.
		DamageAmp = 1.f;
	}

	float Raw = (BaseDamage + AttackPower) * DamageAmp;

	// --- 1. Healing bypass ----------------------------------------------------
	// Checked here rather than first only because it needs Raw computed. Behaves
	// as the early branch in receive_damage(): no armor, no resistance, no
	// interception, and it routes to a different meta attribute.
	if (DamageType && DamageType->HasCategory(TAG_Damage_Category_Healing))
	{
		if (Raw > 0.f)
		{
			OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(
				UARPGVitalSet::GetIncomingHealingAttribute(), EGameplayModOp::Additive, Raw));
		}
		return;
	}

	// --- 3. Crit --------------------------------------------------------------
	// The roll itself happened in the hitbox, server-side, and rode here on the
	// context. Rolling it here would be equally authoritative but would desync
	// from the bIsCritical the client already received for its hit reaction.
	if (Context && Context->bIsCritical)
	{
		float CritMultiplier = 1.f;
		ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(S.CritMultiplierDef, EvalParams, CritMultiplier);
		Raw *= FMath::Max(1.f, CritMultiplier);
	}

	// --- 4. Armor (physical only, flat, before resistance) --------------------
	if (DamageType && DamageType->HasCategory(TAG_Damage_Category_Physical))
	{
		float BaseArmor = 0.f;
		ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(S.BaseArmorDef, EvalParams, BaseArmor);
		Raw = FMath::Max(0.f, Raw - FMath::Max(0.f, BaseArmor));
	}

	// --- 5. Resistance --------------------------------------------------------
	float EffectiveResistance = 0.f;
	if (DamageType && !DamageType->HasCategory(TAG_Damage_Category_True))
	{
		float RawResistance = 0.f;

		// An unset ResistanceAttribute is legitimate -- fall damage is resisted
		// by nothing -- so this is not a warning case.
		if (DamageType->ResistanceAttribute.IsValid())
		{
			if (const FGameplayEffectAttributeCaptureDefinition* CaptureDef =
					S.ResistanceCaptures.Find(DamageType->ResistanceAttribute))
			{
				ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(*CaptureDef, EvalParams, RawResistance);
			}
			else
			{
				// The asset names an attribute this execution does not capture,
				// which means the three-line checklist at the top of this file
				// was only half done. Silently dealing full damage would hide it.
				UE_LOG(LogARPGCombat, Warning,
					TEXT("Damage type '%s' names resistance attribute '%s', which UARPGDamageExecution ")
					TEXT("does not capture. Treating resistance as 0. Add it to FARPGDamageStatics."),
					*GetNameSafe(DamageType), *DamageType->ResistanceAttribute.GetName());
			}
		}

		const float Penetration = (Context && Context->Penetration >= 0.f)
			? Context->Penetration
			: DamageType->DefaultPenetration;

		EffectiveResistance = FMath::Clamp(
			RawResistance * (1.f - FMath::Clamp(Penetration, 0.f, 1.f)),
			DamageType->MinResistance,
			1.f);
	}
	else if (!DamageType)
	{
		UE_LOG(LogARPGCombat, Warning,
			TEXT("Damage effect '%s' has no UARPGDamageTypeAsset on its context; ")
			TEXT("dealing unmitigated damage. The hitbox should always stamp one."),
			*GetNameSafe(Spec.Def));
	}

	// --- 6. Final -------------------------------------------------------------
	float Final = Raw * (1.f - EffectiveResistance);

	// PHASE 3 INSERTS HERE: parry/block interception multiplies Final, and a
	// successful intercept redirects poise damage to the ATTACKER's meter
	// instead of the victim's (see the poise block below).
	// PHASE 5 INSERTS AFTER THAT: cloak absorption subtracts from Final last,
	// measured against the already-mitigated number.

	Final = FMath::Max(0.f, Final);

	if (Final > 0.f)
	{
		OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(
			UARPGVitalSet::GetIncomingDamageAttribute(), EGameplayModOp::Additive, Final));
	}

	// --- Poise ----------------------------------------------------------------
	// Rides the same execution so a hit and its stagger contribution land
	// atomically. In Godot these were sequential statements in receive_damage();
	// splitting them across two effects would let one apply without the other.
	if (Context && Context->PoiseDamage > 0.f)
	{
		OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(
			UARPGVitalSet::GetIncomingPoiseDamageAttribute(), EGameplayModOp::Additive, Context->PoiseDamage));
	}

	// --- Per-type proc hook ---------------------------------------------------
	if (DamageType)
	{
		if (const UAbilitySystemComponent* TargetASC = ExecutionParams.GetTargetAbilitySystemComponent())
		{
			DamageType->OnDamageApplied(TargetASC->GetAvatarActor(), Final, Spec.GetContext());
		}
	}
}
