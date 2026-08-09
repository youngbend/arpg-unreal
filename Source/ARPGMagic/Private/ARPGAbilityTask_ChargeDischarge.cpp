// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGAbilityTask_ChargeDischarge.h"
#include "ARPGMagic.h"
#include "ARPGMagicComponent.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"

UARPGAbilityTask_ChargeDischarge::UARPGAbilityTask_ChargeDischarge()
{
	bTickingTask = true;
}

UARPGAbilityTask_ChargeDischarge* UARPGAbilityTask_ChargeDischarge::ChargeDischarge(
	UGameplayAbility* OwningAbility, UARPGMagicComponent* MagicComponent,
	EARPGDischargeType InDischargeType, float InMaxChargeTime, bool bInPayMinimumUpFront)
{
	UARPGAbilityTask_ChargeDischarge* Task = NewAbilityTask<UARPGAbilityTask_ChargeDischarge>(OwningAbility);
	Task->Magic = MagicComponent;
	Task->DischargeType = InDischargeType;
	Task->MaxChargeTime = FMath::Max(KINDA_SMALL_NUMBER, InMaxChargeTime);
	Task->bPayMinimumUpFront = bInPayMinimumUpFront;
	return Task;
}

float UARPGAbilityTask_ChargeDischarge::CostForFraction(float Fraction) const
{
	return UsageRate * FMath::Lerp(MinCost, MaxCost, FMath::Clamp(Fraction, 0.f, 1.f));
}

float UARPGAbilityTask_ChargeDischarge::FractionForCost(float Cost) const
{
	const float Span = MaxCost - MinCost;
	if (UsageRate <= 0.f || Span <= 0.f)
	{
		// Nothing to solve: either the spell is free or its cost does not vary
		// with charge, in which case the mana spent says nothing about how far
		// the charge got.
		return 0.f;
	}

	return FMath::Clamp((Cost / UsageRate - MinCost) / Span, 0.f, 1.f);
}

void UARPGAbilityTask_ChargeDischarge::Activate()
{
	Super::Activate();

	if (!Magic)
	{
		UE_LOG(LogARPGMagic, Warning, TEXT("ChargeDischarge task started with no magic component."));
		if (ShouldBroadcastAbilityTaskDelegates())
		{
			OnFailed.Broadcast(0.f, false);
		}
		EndTask();
		return;
	}

	// Captured once. A weapon swap or a selection change mid-charge must not
	// reprice a charge already being paid for.
	const FARPGDischargeTypeSettings Settings = Magic->GetDischargeSettings(DischargeType);
	UsageRate = Magic->GetUsageRate();
	MinCost = Settings.MinManaCost;
	MaxCost = Settings.MaxManaCost;

	if (bPayMinimumUpFront)
	{
		const float Initial = CostForFraction(0.f);
		if (Initial > 0.f && !Magic->TrySpendManaForCharge(Initial))
		{
			// Cannot afford even the minimum, so the charge never starts. The
			// ability should not have activated -- this is the backstop for the
			// case where mana was drained between the cost check and here.
			if (ShouldBroadcastAbilityTaskDelegates())
			{
				OnFailed.Broadcast(0.f, false);
			}
			EndTask();
			return;
		}
		Spent = Initial;
	}
}

void UARPGAbilityTask_ChargeDischarge::TickTask(float DeltaTime)
{
	Super::TickTask(DeltaTime);

	if (bReleased || !Magic)
	{
		return;
	}

	Elapsed += DeltaTime;
	const float Fraction = FMath::Clamp(Elapsed / MaxChargeTime, 0.f, 1.f);

	// What this fraction should have cost in TOTAL, minus what has already been
	// paid. Cumulative rather than per-frame, so the price of a given charge does
	// not depend on frame rate.
	const float TargetSpend = CostForFraction(Fraction);
	const float Delta = TargetSpend - Spent;

	if (Delta > 0.f)
	{
		const float Available = Magic->GetAvailableMana();

		if (Available < Delta)
		{
			// Running dry does not fail the spell -- it fires early at whatever
			// the caster could actually pay for. Spend the remainder so the
			// solved fraction reflects real expenditure rather than leaving a
			// sliver behind that would make the cast look cheaper than it was.
			if (Available > 0.f)
			{
				Magic->TrySpendManaForCharge(Available);
				Spent += Available;
			}

			bReleased = true;
			if (ShouldBroadcastAbilityTaskDelegates())
			{
				OnReleased.Broadcast(FractionForCost(Spent), /*bForced=*/true);
			}
			EndTask();
			return;
		}

		Magic->TrySpendManaForCharge(Delta);
		Spent = TargetSpend;
	}

	// Held to full. Releasing here rather than idling at 100% means the caster
	// is not silently paying nothing while holding a finished charge, and the
	// spell goes off the moment it is ready.
	if (Fraction >= 1.f)
	{
		bReleased = true;
		if (ShouldBroadcastAbilityTaskDelegates())
		{
			OnReleased.Broadcast(1.f, /*bForced=*/false);
		}
		EndTask();
	}
}

void UARPGAbilityTask_ChargeDischarge::ReleaseCharge()
{
	if (bReleased)
	{
		return;
	}
	bReleased = true;

	const float Fraction = FMath::Clamp(Elapsed / MaxChargeTime, 0.f, 1.f);
	if (ShouldBroadcastAbilityTaskDelegates())
	{
		OnReleased.Broadcast(Fraction, /*bForced=*/false);
	}
	EndTask();
}

void UARPGAbilityTask_ChargeDischarge::OnDestroy(bool bInOwnerFinished)
{
	// Mana already spent is deliberately not refunded on an abandoned charge:
	// the caster committed it, and refunding would make interrupting your own
	// charge a way to cast for free.
	Super::OnDestroy(bInOwnerFinished);
}
