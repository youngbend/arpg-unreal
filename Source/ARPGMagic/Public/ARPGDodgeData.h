// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ARPGDodgeData.generated.h"

class UAnimMontage;

/**
 * The player's standard, non-elemental dodge. Port of Godot's StandardDodgeData.
 *
 * The i-frame window is a START and a DURATION rather than a single number,
 * because a dodge that is invincible from frame zero and a dodge that has to be
 * timed are different defensive skills. Duration 0 means "the whole dodge",
 * which is the forgiving default.
 */
UCLASS(BlueprintType)
class ARPGMAGIC_API UARPGStandardDodgeData : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Animation")
	TObjectPtr<UAnimMontage> Montage;

	/** Played when dodging with no directional input. Falls back to Montage. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Animation")
	TObjectPtr<UAnimMontage> BackstepMontage;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement",
		meta = (ClampMin = "0.0"))
	float Distance = 400.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement",
		meta = (ClampMin = "0.001"))
	float Duration = 0.25f;

	/** Degrees per second the character turns to face the dodge direction. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement",
		meta = (ClampMin = "0.0"))
	float RotationSpeed = 460.f;

	/** Cap on steering during a backstep, degrees per second. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement",
		meta = (ClampMin = "0.0"))
	float MaxBackstepTurnSpeed = 172.f;

	/** Seconds before i-frames begin. 0 = immediately. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Defence",
		meta = (ClampMin = "0.0"))
	float InvincibilityWindowStart = 0.f;

	/** How long i-frames last. 0 = the whole dodge. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Defence",
		meta = (ClampMin = "0.0"))
	float InvincibilityWindowDuration = 0.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Cost",
		meta = (ClampMin = "0.0"))
	float StaminaCost = 15.f;
};

/**
 * How one element's dodge behaves. Port of Godot's ElementalDodgeData.
 *
 * Assigned to UARPGMagicElement::ElementalDodge. Leave unset and the element
 * simply has no dodge of its own -- consuming it for a dodge then falls back to
 * the standard one, rather than producing a broken elemental version.
 *
 * IMPULSE IS IN DODGE-RELATIVE SPACE, not world or actor space:
 *   +X = the dodge direction, +Y = right of it, +Z = world up.
 * So (800, 0, 0) is a hard burst along the dodge (a fire jet), (300, 0, 400) is
 * a leaping diagonal (a wind hop), and zero is no impulse at all, for a
 * teleport the effect handles itself. It is additive to the base dodge velocity
 * and decays linearly across the duration.
 */
UCLASS(BlueprintType)
class ARPGMAGIC_API UARPGElementalDodgeData : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Travel distance. 0 for teleport-style dodges that reposition the caster. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement",
		meta = (ClampMin = "0.0"))
	float Distance = 400.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement",
		meta = (ClampMin = "0.001"))
	float Duration = 0.25f;

	/** Additive burst in dodge-relative space -- see the class comment. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement")
	FVector Impulse = FVector::ZeroVector;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement",
		meta = (ClampMin = "0.0"))
	float RotationSpeed = 460.f;

	/**
	 * Lock the direction at the start. False lets the player steer mid-dodge at
	 * up to MaxTurnSpeed -- what makes a wind dash feel different from a
	 * committed fire lunge.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement")
	bool bLockDirection = true;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement",
		meta = (EditCondition = "!bLockDirection", ClampMin = "0.0"))
	float MaxTurnSpeed = 172.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement")
	bool bAllowAirDodge = false;

	/** Seconds of invincibility from the start. 0 = the whole dodge. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Defence",
		meta = (ClampMin = "0.0"))
	float InvincibilityWindow = 0.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Animation")
	TObjectPtr<UAnimMontage> Montage;

	/** Spawned at the dodge origin. Owns its own hitbox, particles and lifetime. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Animation")
	TSoftClassPtr<AActor> DodgeEffect;

	/** Scales the element's BaseDamage for this dodge's hitbox. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage",
		meta = (ClampMin = "0.0"))
	float DamageMultiplier = 0.5f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Cost",
		meta = (ClampMin = "0.0"))
	float StaminaCost = 20.f;
};
