// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGPoiseComponent.h"
#include "Engine/DataAsset.h"
#include "ARPGReactionDefinitions.generated.h"

class UAnimMontage;

/**
 * Per-weapon block and parry timing and animation. Assign to
 * UARPGWeaponAttackTree::Block.
 *
 * THE TIMINGS BELONG TO THE WEAPON, not to the character. That is the whole
 * reason this exists as an asset rather than as settings on the parry
 * component: a buckler snaps up and offers a narrow, forgiving parry; a
 * greatshield takes a beat to raise and then covers everything. Two characters
 * with identical stats should defend differently because they are holding
 * different things.
 *
 * The timeline on the block button going down:
 *
 *   [0, BlendInTime)                          raising the guard. NOT yet
 *                                             blocking, and NOT yet parrying --
 *                                             a hit here lands clean.
 *   [BlendInTime, +ParryWindow)               the perfect-parry window.
 *   [BlendInTime + ParryWindow, ...)          still blocking; hits are reduced
 *                                             rather than turned.
 *
 * Every montage may be left null. The parry component drives the mechanics off
 * the timings alone, so blocking, parrying and their damage consequences all
 * work correctly with no clips authored yet -- which is what lets combat be
 * tuned before it is animated.
 */
UCLASS(BlueprintType)
class ARPGCOMBAT_API UARPGBlockDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Seconds to raise the guard before the parry window opens. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Timing", meta = (ClampMin = "0.0"))
	float BlendInTime = 0.10f;

	/** How long the perfect parry stays open once the guard is up. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Timing", meta = (ClampMin = "0.0"))
	float ParryWindow = 0.15f;

	/**
	 * Movement multiplier while holding this block. 1 is unrestricted, 0 roots.
	 *
	 * The same knob attacks use, and for the same reason: what a weapon costs
	 * you in mobility while you hold it up is part of what distinguishes it.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Timing",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MovementSpeedFactor = 0.6f;

	/** Held while blocking. Looped by the animation layer if it is short. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Animation")
	TObjectPtr<UAnimMontage> BlockReadyMontage;

	/** One-shot, on a hit turned during the parry window. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Animation")
	TObjectPtr<UAnimMontage> ParryMontage;

	/** One-shots, on a hit blocked rather than parried. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Animation")
	TObjectPtr<UAnimMontage> BlockLightMontage;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Animation")
	TObjectPtr<UAnimMontage> BlockHeavyMontage;
};

/**
 * Per-weapon poise reactions. Assign to UARPGWeaponAttackTree::Flinch.
 *
 * Played in response to the poise component's three tiers -- see its class
 * comment for what each one actually does to the character. This asset carries
 * only the clips: the mechanics (the attack-start lock on a light flinch, the
 * cancel on a heavy one, the full stun on a stance break) are the poise
 * component's, and they run whether or not anything here is filled in.
 *
 * Per WEAPON rather than per character because a reaction is a reaction WITH
 * SOMETHING IN YOUR HANDS. Staggering while holding a greatsword does not look
 * like staggering while holding a dagger, and the alternative -- one set of
 * reactions on the character -- means every weapon flinches identically.
 */
UCLASS(BlueprintType)
class ARPGCOMBAT_API UARPGFlinchDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/**
	 * A modest poise hit. An OVERLAY: the legs keep walking, because a light
	 * flinch does not interrupt what the character is doing -- it only stops
	 * them starting something new for a moment.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Animation")
	TObjectPtr<UAnimMontage> LightFlinchMontage;

	/** A single big hit. Full body, and it cancels an attack in progress. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Animation")
	TObjectPtr<UAnimMontage> HeavyFlinchMontage;

	/** The meter FILLED. Full body, and the character is open throughout. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Animation")
	TObjectPtr<UAnimMontage> StanceBreakMontage;

	/** The montage for a result, or null if none is authored for it. */
	UAnimMontage* GetMontageFor(EARPGPoiseResult Result) const;
};
