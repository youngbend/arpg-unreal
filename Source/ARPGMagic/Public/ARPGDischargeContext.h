// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "ARPGMagicTypes.h"
#include "ARPGDischargeContext.generated.h"

class AActor;
class UARPGMagicElement;

/**
 * Everything one cast is, snapshotted at the moment of release. Port of Godot's
 * DischargeContext.
 *
 * A struct rather than an object because it is a value handed to a spawned
 * effect and then read: it has no identity, and copying it into the effect actor
 * is exactly what should happen.
 *
 * POWER AND DAMAGE ARE TWO INDEPENDENT CURVES over charge, and neither feeds the
 * other. That separation is the reason both are precomputed here rather than
 * left for the effect to work out:
 *
 *   PowerFraction is COSMETIC ONLY. It drives how big and bright the effect is,
 *   in the same units as MinPower/MaxPower -- and is deliberately NOT clamped to
 *   0..1 despite the name, because mastery can push it above MaxPower and a
 *   debuff below MinPower. Effects normalise it themselves:
 *       t = clamp((PowerFraction - MinPower) / (MaxPower - MinPower), 0, 1)
 *   Being decoupled from damage is what makes it comparable across every element
 *   and discharge type: a debuffed full charge visibly looks weaker instead of
 *   just quietly dealing less, and a master's tap-cast looks impressive.
 *
 *   ComputedDamage is the actual number dealt, with its own charge ratio and its
 *   own mastery treatment (a genuine multiplier rather than an additive window
 *   shift).
 *
 * Tuning one can therefore never accidentally move the other.
 */
USTRUCT(BlueprintType)
struct ARPGMAGIC_API FARPGDischargeContext
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	EARPGDischargeType DischargeType = EARPGDischargeType::Burst;

	/** Who cast it. Damage attribution and faction filtering both need this. */
	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	TObjectPtr<AActor> Caster = nullptr;

	/**
	 * The combination result when one resolved, otherwise the single active
	 * primitive. Damage type, status and VFX all come from here.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	TObjectPtr<UARPGMagicElement> PrimaryElement = nullptr;

	/**
	 * Non-null only for a resolved combination. Same as PrimaryElement in that
	 * case, and provided separately so an effect can tell a combination cast
	 * from a solo one without re-running the table.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	TObjectPtr<UARPGMagicElement> CombinationElement = nullptr;

	/** Every primitive that was active, for effects that care about the mix. */
	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	FGameplayTagContainer ActiveElements;

	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	FVector Origin = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	FVector Direction = FVector::ForwardVector;

	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	FVector AimTarget = FVector::ZeroVector;

	/** 0-1, how far the charge actually got. */
	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	float Charge = 0.f;

	/** True when the charge ended because mana ran out rather than on release. */
	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	bool bForcedRelease = false;

	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	bool bWeaponIsCatalyst = false;

	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	float BaseWeaponPower = 0.f;

	/**
	 * The final damage. Effects stamp this straight onto their hitbox rather
	 * than re-deriving it.
	 *
	 * For a "defensive"-tagged element the cloak reuses this as shield capacity
	 * instead; for a "healing"-tagged one the effect's hitbox should carry a
	 * healing damage type, and the execution then treats this as heal power. Both
	 * reuse the ordinary hit pipeline rather than adding a parallel one.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	float ComputedDamage = 0.f;

	/** Poise counterpart, scaled by charge and type but never by mastery. */
	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	float ComputedPoiseDamage = 0.f;

	/** The window PowerFraction lives in, for effects to normalise against. */
	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	float MinPower = 0.f;

	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	float MaxPower = 0.f;

	/** Cosmetic output level -- see the struct comment. Never affects damage. */
	UPROPERTY(BlueprintReadWrite, Category = "Discharge")
	float PowerFraction = 0.f;

	/** Normalised 0-1 position of PowerFraction within the power window. */
	float GetNormalisedPower() const
	{
		const float Span = MaxPower - MinPower;
		return Span > KINDA_SMALL_NUMBER
			? FMath::Clamp((PowerFraction - MinPower) / Span, 0.f, 1.f)
			: 0.f;
	}
};
