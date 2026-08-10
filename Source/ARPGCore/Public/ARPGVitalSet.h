// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGAttributeSetBase.h"
#include "ARPGVitalSet.generated.h"

/**
 * Broadcast with the size of a SINGLE poise contribution, before anything has
 * been accumulated.
 *
 * The size of one hit is what decides a flinch tier -- not the running total --
 * so it has to be handed on rather than folded into the attribute first.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FARPGOnPoiseDamageReceived, float /*Amount*/);

/**
 * Health crossed zero (true) or came back above it (false).
 *
 * Fired only on the transition, never on a repeat: a corpse taking three more
 * hits from a swing already in flight is one death, not four.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FARPGOnDeathStateChanged, bool /*bDead*/);

/**
 * Health, stamina, mana and poise.
 *
 * Replaces Godot's CombatComponent (health) and ResourceComponent (stamina/mana)
 * outright: spend_stamina/spend_mana become ability costs, and
 * restore_*_over_time becomes a periodic GameplayEffect, so the hand-rolled
 * timed-regen vectors in ResourceComponent have no equivalent here by design.
 *
 * POISE ACCUMULATES UPWARD. It is not a second health bar. Following the Godot
 * PoiseComponent, the meter starts at 0, fills as hits land, triggers a stance
 * break on reaching MaxPoise (then resets to 0), and drains back toward 0 after
 * a hold window. Phase 3 adds the flinch tiers and the hold-then-drain decay;
 * this set only owns the number.
 */
UCLASS()
class ARPGCORE_API UARPGVitalSet : public UARPGAttributeSetBase
{
	GENERATED_BODY()

public:
	UARPGVitalSet();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;

	// --- Health ---------------------------------------------------------------
	UPROPERTY(BlueprintReadOnly, Category = "Vitals", ReplicatedUsing = OnRep_Health)
	FGameplayAttributeData Health;
	ATTRIBUTE_ACCESSORS(UARPGVitalSet, Health)

	UPROPERTY(BlueprintReadOnly, Category = "Vitals", ReplicatedUsing = OnRep_MaxHealth)
	FGameplayAttributeData MaxHealth;
	ATTRIBUTE_ACCESSORS(UARPGVitalSet, MaxHealth)

	// --- Stamina --------------------------------------------------------------
	UPROPERTY(BlueprintReadOnly, Category = "Vitals", ReplicatedUsing = OnRep_Stamina)
	FGameplayAttributeData Stamina;
	ATTRIBUTE_ACCESSORS(UARPGVitalSet, Stamina)

	UPROPERTY(BlueprintReadOnly, Category = "Vitals", ReplicatedUsing = OnRep_MaxStamina)
	FGameplayAttributeData MaxStamina;
	ATTRIBUTE_ACCESSORS(UARPGVitalSet, MaxStamina)

	/** Attributes rather than config so a consumable or debuff can move them. */
	UPROPERTY(BlueprintReadOnly, Category = "Vitals", ReplicatedUsing = OnRep_StaminaRegenRate)
	FGameplayAttributeData StaminaRegenRate;
	ATTRIBUTE_ACCESSORS(UARPGVitalSet, StaminaRegenRate)

	// --- Mana -----------------------------------------------------------------
	UPROPERTY(BlueprintReadOnly, Category = "Vitals", ReplicatedUsing = OnRep_Mana)
	FGameplayAttributeData Mana;
	ATTRIBUTE_ACCESSORS(UARPGVitalSet, Mana)

	UPROPERTY(BlueprintReadOnly, Category = "Vitals", ReplicatedUsing = OnRep_MaxMana)
	FGameplayAttributeData MaxMana;
	ATTRIBUTE_ACCESSORS(UARPGVitalSet, MaxMana)

	UPROPERTY(BlueprintReadOnly, Category = "Vitals", ReplicatedUsing = OnRep_ManaRegenRate)
	FGameplayAttributeData ManaRegenRate;
	ATTRIBUTE_ACCESSORS(UARPGVitalSet, ManaRegenRate)

	// --- Poise ----------------------------------------------------------------
	UPROPERTY(BlueprintReadOnly, Category = "Vitals", ReplicatedUsing = OnRep_Poise)
	FGameplayAttributeData Poise;
	ATTRIBUTE_ACCESSORS(UARPGVitalSet, Poise)

	UPROPERTY(BlueprintReadOnly, Category = "Vitals", ReplicatedUsing = OnRep_MaxPoise)
	FGameplayAttributeData MaxPoise;
	ATTRIBUTE_ACCESSORS(UARPGVitalSet, MaxPoise)

	// --- Meta attributes ------------------------------------------------------
	// Never replicated and never persisted: an execution writes one,
	// PostGameplayEffectExecute consumes it and zeroes it in the same call.
	// Reading these anywhere else gives whatever the last hit left behind.

	UPROPERTY(BlueprintReadOnly, Category = "Vitals|Meta")
	FGameplayAttributeData IncomingDamage;
	ATTRIBUTE_ACCESSORS(UARPGVitalSet, IncomingDamage)

	UPROPERTY(BlueprintReadOnly, Category = "Vitals|Meta")
	FGameplayAttributeData IncomingHealing;
	ATTRIBUTE_ACCESSORS(UARPGVitalSet, IncomingHealing)

	UPROPERTY(BlueprintReadOnly, Category = "Vitals|Meta")
	FGameplayAttributeData IncomingPoiseDamage;
	ATTRIBUTE_ACCESSORS(UARPGVitalSet, IncomingPoiseDamage)

	/**
	 * Fired when poise damage arrives. This set deliberately does NOT accumulate
	 * it into the Poise attribute.
	 *
	 * Resolving poise means owning flinch thresholds, a stance-break immunity
	 * window and hold-then-drain decay -- all of which live in
	 * UARPGPoiseComponent over in ARPGCombat, which sits ABOVE this module and
	 * cannot be reached from here. Handing the raw contribution over keeps the
	 * dependency pointing one way, and keeps an attribute set doing what an
	 * attribute set should.
	 *
	 * A character with no poise component therefore has no stance, which is the
	 * correct behaviour rather than a gap.
	 */
	FARPGOnPoiseDamageReceived OnPoiseDamageReceived;

	/**
	 * Health reached zero, or recovered from it.
	 *
	 * Raised alongside -- not instead of -- the State.Dead tag and the
	 * Event.Death gameplay event, both of which this set applies itself. The tag
	 * is what every ActivationBlockedTags entry and every IsDead() guard in the
	 * project already reads; the event is what a death ability triggers on. They
	 * were declared in phase 0 and granted by nothing until now, which meant all
	 * of that gating was inert.
	 *
	 * Applied HERE rather than in a component because this is the only place that
	 * sees the crossing at the moment it happens, and because a character with no
	 * death component should still stop fighting when it dies.
	 */
	FARPGOnDeathStateChanged OnDeathStateChanged;

	/** True once health has reached zero, until it is restored above zero. */
	bool IsDead() const { return bDead; }

protected:
	UFUNCTION() void OnRep_Health(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_MaxHealth(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_Stamina(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_MaxStamina(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_StaminaRegenRate(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_Mana(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_MaxMana(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_ManaRegenRate(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_Poise(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_MaxPoise(const FGameplayAttributeData& OldValue);

private:
	/**
	 * Keeps a current value proportional when its maximum moves, so a
	 * progression point that raises MaxHealth doesn't leave the character at a
	 * lower percentage than before.
	 */
	void AdjustAttributeForMaxChange(
		const FGameplayAttributeData& AffectedAttribute,
		const FGameplayAttributeData& MaxAttribute,
		float NewMaxValue,
		const FGameplayAttribute& AffectedAttributeProperty) const;

	/**
	 * Applies or lifts State.Dead when health crosses zero, and raises
	 * Event.Death on the way down.
	 *
	 * Idempotent: called after every change to Health from any direction, and a
	 * no-op unless the dead/alive answer actually moved.
	 */
	void RefreshDeathState();

	/** Latched, so the transition can be distinguished from the state. */
	bool bDead = false;
};
