// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "ARPGGameplayAbility_MeleeAttack.generated.h"

class UARPGAttackDefinition;
class UARPGComboComponent;
class UARPGHitboxComponent;

/**
 * Performs one beat of a combo. Triggered by Event.Attack.Begin, with the
 * resolved UARPGAttackDefinition riding on the event's OptionalObject.
 *
 * THE DIVISION OF LABOUR. UARPGComboComponent owns tree position, buffering and
 * the reset timer, and decides WHICH attack happens. This owns what happens once
 * that decision is made: the montage, the tags, and the hitbox window. Neither
 * reaches into the other's state.
 *
 * STAMINA IS NOT CHARGED HERE, despite cost normally belonging to the ability.
 * The combo component has to know whether an attack can be afforded BEFORE it
 * advances the tree -- an unaffordable swing must leave the chain untouched
 * rather than consuming a beat. That check and the spend cannot be separated
 * without a window where the tree has moved but the attack has not happened, so
 * they live together in the component. Charging again here would double-bill.
 *
 * HYPERARMOR IS PER-ATTACK, NOT PER-ABILITY, so State.Hyperarmor is granted as a
 * loose tag for the attack's duration rather than declared in
 * ActivationOwnedTags -- the same ability class serves armoured and unarmoured
 * swings alike.
 */
UCLASS()
class ARPGCOMBAT_API UARPGGameplayAbility_MeleeAttack : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UARPGGameplayAbility_MeleeAttack();

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility, bool bWasCancelled) override;

protected:
	UFUNCTION() void OnHitboxOn(FGameplayEventData Payload);
	UFUNCTION() void OnHitboxOff(FGameplayEventData Payload);
	UFUNCTION() void OnComboWindow(FGameplayEventData Payload);
	UFUNCTION() void OnMontageFinished();
	UFUNCTION() void OnMontageCancelled();

private:
	UARPGHitboxComponent* ResolveHitbox() const;
	UARPGComboComponent* ResolveCombo() const;

	/** Stamps the swing's payload onto the hitbox and arms it. */
	void ArmHitbox(int32 WindowIndex, bool bLandingWindow);
	void DisarmHitbox();

	/** Puts back anything the attack changed on the character. */
	void RestoreCharacter();

	UPROPERTY(Transient)
	TObjectPtr<const UARPGAttackDefinition> CurrentAttack;

	UPROPERTY(Transient)
	TObjectPtr<UARPGHitboxComponent> ArmedHitbox;

	/** Restored on end, so a rooted attack does not leave the character slowed. */
	float CachedMaxWalkSpeed = 0.f;
	bool bAppliedSpeedFactor = false;
	bool bGrantedHyperarmor = false;

	/** Guards against the combo window notify firing twice on a looping montage. */
	bool bComboWindowOpened = false;
};
