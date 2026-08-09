// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGModalInputComponent.h"
#include "ARPGModalInputTestListener.generated.h"

/**
 * Records everything a modal input component broadcasts, in order.
 *
 * A UObject with real UFUNCTIONs rather than a lambda, because the component's
 * events are DYNAMIC delegates -- AddDynamic needs a named UFUNCTION on a
 * UObject, and there is no AddLambda to reach for. Since the whole output of an
 * input component is its events, there is no state to assert on instead: what a
 * button press did IS what it broadcast.
 *
 * Events are flattened to strings with their parameters baked in ("MagicSelect:1")
 * so a test can assert on both the event and its argument in one comparison, and
 * so ORDER is checkable -- which matters for the rules about what a press
 * swallows.
 */
UCLASS()
class UARPGModalInputTestListener : public UObject
{
	GENERATED_BODY()

public:
	/** Every event broadcast since the last Clear(), oldest first. */
	TArray<FString> Events;

	void Bind(UARPGModalInputComponent* Input);

	void Clear() { Events.Reset(); }

	bool Saw(const FString& Event) const { return Events.Contains(Event); }

	int32 CountOf(const FString& Event) const
	{
		int32 Total = 0;
		for (const FString& Recorded : Events)
		{
			if (Recorded == Event)
			{
				++Total;
			}
		}
		return Total;
	}

	/** Everything recorded, comma-joined -- for a failure message worth reading. */
	FString Describe() const { return FString::Join(Events, TEXT(", ")); }

	UFUNCTION() void OnLightAttack() { Events.Add(TEXT("Light")); }
	UFUNCTION() void OnLightAttackReleased() { Events.Add(TEXT("LightReleased")); }
	UFUNCTION() void OnHeavyAttack() { Events.Add(TEXT("Heavy")); }
	UFUNCTION() void OnHeavyAttackReleased() { Events.Add(TEXT("HeavyReleased")); }
	UFUNCTION() void OnSpecialAttack() { Events.Add(TEXT("Special")); }
	UFUNCTION() void OnSpecialAttackReleased() { Events.Add(TEXT("SpecialReleased")); }
	UFUNCTION() void OnJump() { Events.Add(TEXT("Jump")); }
	UFUNCTION() void OnDodge() { Events.Add(TEXT("Dodge")); }
	UFUNCTION() void OnParryPressed() { Events.Add(TEXT("ParryPressed")); }
	UFUNCTION() void OnParryReleased() { Events.Add(TEXT("ParryReleased")); }
	UFUNCTION() void OnSheathe() { Events.Add(TEXT("Sheathe")); }
	UFUNCTION() void OnMagicDiscard() { Events.Add(TEXT("MagicDiscard")); }
	UFUNCTION() void OnMagicPagePrev() { Events.Add(TEXT("MagicPagePrev")); }
	UFUNCTION() void OnMagicPageNext() { Events.Add(TEXT("MagicPageNext")); }
	UFUNCTION() void OnQuickSlotPrev() { Events.Add(TEXT("QuickSlotPrev")); }
	UFUNCTION() void OnQuickSlotNext() { Events.Add(TEXT("QuickSlotNext")); }
	UFUNCTION() void OnQuickSlotUse() { Events.Add(TEXT("QuickSlotUse")); }
	UFUNCTION() void OnDischargeModifierPressed() { Events.Add(TEXT("DischargeModifier")); }
	UFUNCTION() void OnDischargeCancelled() { Events.Add(TEXT("DischargeCancelled")); }

	UFUNCTION()
	void OnMagicSelect(EARPGInputFace Slot)
	{
		Events.Add(FString::Printf(TEXT("MagicSelect:%d"), static_cast<int32>(Slot)));
	}

	UFUNCTION()
	void OnDischargeChargeStarted(EARPGInputFace Slot)
	{
		Events.Add(FString::Printf(TEXT("ChargeStarted:%d"), static_cast<int32>(Slot)));
	}

	UFUNCTION()
	void OnDischargeActivated(EARPGInputFace Slot, float Charge)
	{
		// Two decimals: the charge is a ratio of elapsed time to a configured
		// maximum, so pinning it exactly would test the tick loop's float
		// accumulation rather than the rule under test.
		Events.Add(FString::Printf(TEXT("Discharge:%d@%.2f"), static_cast<int32>(Slot), Charge));
	}
};
