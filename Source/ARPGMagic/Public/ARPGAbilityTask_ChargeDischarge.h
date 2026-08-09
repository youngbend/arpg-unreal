// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/Tasks/AbilityTask.h"
#include "ARPGMagicTypes.h"
#include "ARPGAbilityTask_ChargeDischarge.generated.h"

class UARPGMagicComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FARPGChargeReleasedDelegate,
	float, ChargeFraction, bool, bForced);

/**
 * Holds a discharge charge, draining mana as it fills. Port of Godot's
 * begin/update/cancel_discharge_charge trio.
 *
 * WHY THIS IS NOT A GAS COST. CostGameplayEffectClass is one-shot: it is checked
 * and paid at activation. A discharge instead drains CONTINUOUSLY while held,
 * and -- the part no cost effect can express -- when the caster runs dry it does
 * not fail. It fires at whatever fraction was actually paid for. A cost effect
 * can only answer "can you afford this, yes or no", and the answer here is "you
 * can afford this much of it".
 *
 * The task therefore owns the drain, and the ability owns what to do when it
 * ends. Run it alongside a WaitInputRelease task; whichever finishes first
 * releases the spell.
 *
 * SPENDING IS CUMULATIVE, NOT PER-FRAME. Each tick works out what the charge
 * SHOULD have cost by now and pays the difference, rather than charging a rate
 * every frame. That makes the total cost of reaching a given fraction the same
 * regardless of frame rate, which a per-frame rate does not -- and it is why a
 * forced release can solve exactly which fraction the spent mana bought.
 */
UCLASS()
class ARPGMAGIC_API UARPGAbilityTask_ChargeDischarge : public UAbilityTask
{
	GENERATED_BODY()

public:
	UARPGAbilityTask_ChargeDischarge();

	/**
	 * @param MaxChargeTime  Seconds to reach a full charge.
	 * @param bPayMinimumUpFront  Charge the minimum cost immediately, so even a
	 *        tap-cast pays for itself. Matches begin_discharge_charge.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic",
		meta = (HidePin = "OwningAbility", DefaultToSelf = "OwningAbility", BlueprintInternalUseOnly = "true"))
	static UARPGAbilityTask_ChargeDischarge* ChargeDischarge(UGameplayAbility* OwningAbility,
		UARPGMagicComponent* MagicComponent, EARPGDischargeType DischargeType,
		float MaxChargeTime, bool bPayMinimumUpFront = true);

	/** Fired once, on release or on running dry. bForced distinguishes the two. */
	UPROPERTY(BlueprintAssignable)
	FARPGChargeReleasedDelegate OnReleased;

	/** The caster could not even afford the minimum, so no charge began. */
	UPROPERTY(BlueprintAssignable)
	FARPGChargeReleasedDelegate OnFailed;

	/** Let go deliberately. Ends the task at the fraction reached. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	void ReleaseCharge();

	virtual void Activate() override;
	virtual void TickTask(float DeltaTime) override;
	virtual void OnDestroy(bool bInOwnerFinished) override;

private:
	/** Mana that reaching this fraction should have cost in total. */
	float CostForFraction(float Fraction) const;

	/** Inverse of CostForFraction: what fraction the given spend actually buys. */
	float FractionForCost(float Cost) const;

	UPROPERTY()
	TObjectPtr<UARPGMagicComponent> Magic;

	EARPGDischargeType DischargeType = EARPGDischargeType::Burst;
	float MaxChargeTime = 1.f;
	bool bPayMinimumUpFront = true;

	float Elapsed = 0.f;

	/** Captured at activation: a weapon swap mid-charge must not reprice it. */
	float UsageRate = 1.f;
	float MinCost = 0.f;
	float MaxCost = 0.f;

	/** Cumulative mana paid so far this charge. */
	float Spent = 0.f;

	bool bReleased = false;
};
