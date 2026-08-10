// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ARPGVitalRegenComponent.generated.h"

class UAbilitySystemComponent;

/**
 * Refills stamina and mana over time. Port of the passive half of Godot's
 * ResourceComponent.
 *
 * THE TWO POOLS REGENERATE DIFFERENTLY, and the difference is the design:
 *
 *   - **Stamina comes back quickly, but not while you are spending it.** A
 *     delay after the last spend is what makes stamina a resource you manage
 *     across an exchange rather than a number that tops itself up between
 *     swings. Attack into a dodge into another attack and the bar never
 *     recovers; disengage for a moment and it does.
 *   - **Mana trickles constantly and slowly.** It is not meant to be recovered
 *     by waiting -- that is what consumables are for -- so it regenerates fast
 *     enough that a long walk between fights matters and slow enough that
 *     standing still is never the answer.
 *
 * The rates are ATTRIBUTES rather than settings here, so a buff, a debuff or a
 * piece of armour can move them through the ordinary aggregator. This component
 * only decides WHEN they apply.
 *
 * A SPEND IS DETECTED BY WATCHING THE ATTRIBUTE FALL, not by callers announcing
 * it. Stamina is spent from the combo component, the locomotion component, the
 * dodge ability and anything added later; a notify-me contract would be
 * forgotten by exactly one of them and the bug would be a stamina bar that
 * refills mid-combo for reasons nobody could reproduce.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCORE_API UARPGVitalRegenComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGVitalRegenComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/** Seconds after the last stamina spend before it starts coming back. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Regen", meta = (ClampMin = "0.0"))
	float StaminaRegenDelay = 1.5f;

	/** Seconds left before stamina resumes. Zero when it is already regenerating. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Regen")
	float GetStaminaRegenDelayRemaining() const { return StaminaHoldTimer; }

	/** Restarts the post-spend delay. Called automatically when stamina drops. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Regen")
	void NotifyStaminaSpent() { StaminaHoldTimer = StaminaRegenDelay; }

protected:
	UAbilitySystemComponent* GetASC() const;

	/**
	 * Binds the stamina watcher if it is not already bound. Idempotent.
	 *
	 * Called from tick as well as begin play, because a player's ability system
	 * lives on the PlayerState and may not have replicated in yet -- the same
	 * late-arrival problem the poise component solves the same way. Subscribing
	 * only once at begin play would leave such a character with stamina that
	 * refills during a combo, silently.
	 */
	void EnsureSubscribed();

	/** Adds Amount to an attribute, clamped at its maximum. */
	void RegenerateTowards(const struct FGameplayAttribute& Current,
		const struct FGameplayAttribute& Max, float Amount);

private:
	void HandleStaminaChanged(const struct FOnAttributeChangeData& Data);

	UPROPERTY(Transient)
	mutable TObjectPtr<UAbilitySystemComponent> CachedASC;

	float StaminaHoldTimer = 0.f;

	bool bSubscribed = false;
};
