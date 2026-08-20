// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGDamageExecution.h"
#include "ARPGCombat.h"
#include "ARPGCombatLibrary.h"
#include "ARPGAssetManager.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGGameplayEffectContext.h"
#include "ARPGGameplayTags.h"
#include "ARPGOffenseSet.h"
#include "ARPGParryComponent.h"
#include "ARPGPoiseComponent.h"
#include "ARPGResistanceSet.h"
#include "ARPGStatusEffectComponent.h"
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

	// A periodic status effect declares its own damage type and per-stack
	// amount, which take precedence over whatever the hit that applied it
	// carried. Burning burns for fire damage regardless of what lit it.
	const UARPGStatusEffectComponent* StatusInfo =
		Spec.Def ? Spec.Def->FindComponent<UARPGStatusEffectComponent>() : nullptr;

	const UARPGDamageTypeAsset* DamageType = nullptr;
	if (StatusInfo && !StatusInfo->TickDamageType.IsNull())
	{
		DamageType = StatusInfo->TickDamageType.LoadSynchronous();
	}
	if (!DamageType && StatusInfo && StatusInfo->TickDamageTypeTag.IsValid())
	{
		// The ordinary route -- see UARPGStatusEffectComponent::TickDamageTypeTag.
		// Null here means no asset claims that tag, which falls through to the
		// context below exactly as an unset damage type always has.
		DamageType = UARPGAssetManager::FindDamageType(StatusInfo->TickDamageTypeTag);
	}
	if (!DamageType && Context)
	{
		DamageType = Context->DamageType.Get();
	}

	// --- 2. Raw ---------------------------------------------------------------
	// SetByCaller rather than a fixed magnitude: the number comes from the
	// weapon/attack/spell at swing time, exactly as base_amount did in Godot.
	//
	// Scaled by stack count so a periodic status effect deals
	// tick_damage_per_stack * stacks, matching StatusEffectInstance::tick(). A
	// stack count cannot be baked into SetByCaller at spec creation because it
	// changes while the effect is already running. Instant effects have a stack
	// count of 1, so this is a no-op for ordinary hits.
	// A status effect's per-stack amount is authored on the effect; everything
	// else supplies it per-hit through SetByCaller.
	const float PerApplication = (StatusInfo && StatusInfo->TickDamagePerStack > 0.f)
		? StatusInfo->TickDamagePerStack
		: Spec.GetSetByCallerMagnitude(TAG_Data_Damage, /*WarnIfNotFound=*/false, 0.f);

	const float BaseDamage = PerApplication * FMath::Max(1, Spec.GetStackCount());

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

	// --- 6. Resisted, then intercepted ----------------------------------------
	const float ResistedDamage = Raw * (1.f - EffectiveResistance);

	AActor* TargetActor = nullptr;
	if (const UAbilitySystemComponent* TargetAvatarASC = ExecutionParams.GetTargetAbilitySystemComponent())
	{
		TargetActor = TargetAvatarASC->GetAvatarActor();
	}

	// An execution runs wherever its effect is applied, including on a client
	// predicting a hit. Everything below that MUTATES state -- burning the parry
	// window, granting Empowered, pushing poise onto the attacker -- must
	// therefore happen on the server only, or a mispredicted swing would consume
	// a parry the server never saw.
	//
	// The damage number itself is still computed on both sides: that is what
	// prediction is for, and the client needs it to play the right reaction.
	const UAbilitySystemComponent* TargetASCConst = ExecutionParams.GetTargetAbilitySystemComponent();
	const bool bAuthoritative = TargetASCConst && TargetASCConst->IsOwnerActorAuthoritative();

	// Unblockable bypasses the guard entirely -- it must be dodged. Checked
	// before the component is even consulted, so an unblockable hit cannot
	// consume a parry window either.
	EARPGInterceptResult Intercept = EARPGInterceptResult::None;
	float InterceptMultiplier = 1.f;

	UARPGParryComponent* Parry = (Context && Context->bUnblockable)
		? nullptr
		: (TargetActor ? TargetActor->FindComponentByClass<UARPGParryComponent>() : nullptr);

	if (Parry)
	{
		// PeekIntercept on a client: same answer, no side effects. The server's
		// TryIntercept is what actually spends the window.
		Intercept = bAuthoritative ? Parry->TryIntercept() : Parry->PeekIntercept();

		if (Intercept == EARPGInterceptResult::Parried)
		{
			InterceptMultiplier = 1.f - Parry->ParryDamageReduction;
		}
		else if (Intercept == EARPGInterceptResult::Blocked)
		{
			InterceptMultiplier = 1.f - Parry->BlockDamageReduction;
		}
	}

	float Final = FMath::Max(0.f, ResistedDamage * InterceptMultiplier);

	// What the guard actually stopped, measured BEFORE any later absorption.
	// Stamina must be charged for what the block stopped, not for damage a
	// magic shield would have eaten anyway.
	const float Blocked = ResistedDamage - Final;

	// PHASE 5 INSERTS HERE: cloak absorption subtracts from Final last, against
	// the already-mitigated number.

	// --- Interception costs and feedback --------------------------------------
	if (Parry && Intercept != EARPGInterceptResult::None)
	{
		const float StaminaCost = (Intercept == EARPGInterceptResult::Parried)
			? Parry->ParryStaminaCost
			: Blocked * Parry->BlockStaminaMultiplier;

		if (StaminaCost > 0.f)
		{
			// An OUTPUT MODIFIER, not a write to the attribute. This used to read
			// the CURRENT stamina and write it back as the BASE, folding any
			// active stamina modifier permanently into the base; and as a direct
			// write it bypassed the effect pipeline entirely, so nothing could
			// observe, predict or roll it back. The aggregator handles both now,
			// and the vital set clamps it to zero.
			OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(
				UARPGVitalSet::GetStaminaAttribute(), EGameplayModOp::Additive, -StaminaCost));
		}

		// Defensive poise: a successful guard pushes back on the ATTACKER's
		// stance instead of the defender's. This is the pressure that makes
		// blocking an offensive act rather than pure attrition.
		//
		// Server only, and it stays a direct call rather than an output modifier
		// because an execution can only write to its own TARGET -- and the whole
		// point here is that the poise lands on somebody else.
		if (bAuthoritative)
		{
			if (const UAbilitySystemComponent* SourceASC = ExecutionParams.GetSourceAbilitySystemComponent())
			{
				if (AActor* AttackerActor = SourceASC->GetAvatarActor())
				{
					if (UARPGPoiseComponent* AttackerPoise =
							AttackerActor->FindComponentByClass<UARPGPoiseComponent>())
					{
						AttackerPoise->ApplyPoiseDamage(
							Intercept == EARPGInterceptResult::Parried
								? AttackerPoise->ParryPoiseDamage
								: AttackerPoise->BlockPoiseDamage);
					}
				}
			}
		}
	}

	if (Final > 0.f)
	{
		OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(
			UARPGVitalSet::GetIncomingDamageAttribute(), EGameplayModOp::Additive, Final));
	}

	// --- Poise ----------------------------------------------------------------
	// Rides the same execution so a hit and its stagger contribution land
	// atomically. In Godot these were sequential statements in receive_damage();
	// splitting them across two effects would let one apply without the other.
	//
	// Only an UNINTERCEPTED hit fills the victim's meter -- a parried or blocked
	// hit already pushed poise onto the attacker above instead. Applying both
	// would let a defender be staggered by a blow they successfully guarded.
	if (Context && Context->PoiseDamage > 0.f && Intercept == EARPGInterceptResult::None)
	{
		OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(
			UARPGVitalSet::GetIncomingPoiseDamageAttribute(), EGameplayModOp::Additive, Context->PoiseDamage));
	}

	// --- Knockback ------------------------------------------------------------
	// Beside the poise push and for the same reasons: server only, and a direct
	// component call because displacement is not an attribute.
	//
	// An INTERCEPTED hit is not displaced. Standing your ground behind a guard is
	// most of what a guard is for, and a blocked blow that still threw the
	// defender across the room would make blocking read as failure.
	if (bAuthoritative && Context && Intercept == EARPGInterceptResult::None)
	{
		UARPGCombatLibrary::ApplyKnockback(TargetActor, Context->KnockbackDirection,
			Context->KnockbackForce);
	}

	// --- Per-type proc hook ---------------------------------------------------
	// Server only, per its own contract: a proc is Fire rolling to apply Burning,
	// and a client rolling independently would show a status that replication
	// then takes away.
	if (DamageType && bAuthoritative && TargetActor)
	{
		DamageType->OnDamageApplied(TargetActor, Final, Spec.GetContext());
	}
}
