// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "ARPGGameplayAbility_MeleeAttack.generated.h"

class UARPGAttackDefinition;
class UARPGComboComponent;
class UARPGDamageTypeAsset;
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
 *
 * CHARGE AND CHANNEL ARE MONTAGE SECTIONS, NOT SEPARATE CLIPS. Godot's
 * animation.gd played a windup clip, then an active clip, tracking elapsed time
 * by hand to know when each ended. Here one montage carries both as named
 * sections (see ARPGMontageSections) and the ability steers it:
 *
 *   - CHARGE holds on Windup, time-stretched so the section takes exactly
 *     ChargeTime, and jumps to Active on release at whatever fraction was
 *     reached. Because the section knows its own length, the stretch factor is
 *     derived rather than authored -- retiming the clip cannot desync the charge.
 *   - CHANNEL plays Windup once, then links Active to itself so it loops, and
 *     relinks it to Recovery to break out. No polling, no manual re-triggering.
 *
 * Both degrade to plain linear playback on a montage with no named sections, so
 * an attack flagged chargeable against unfinished content still swings.
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

	/** The player let go, or the charge reached full. Cuts to the active window. */
	UFUNCTION() void OnChargeReleased(FGameplayEventData Payload);

	/** The channel should stop looping and fall out to its recovery. */
	UFUNCTION() void OnChannelStop(FGameplayEventData Payload);

	/** The Windup section has played out -- ask the component whether to loop. */
	UFUNCTION() void OnChannelWindupFinished();

private:
	UARPGHitboxComponent* ResolveHitbox() const;
	UARPGComboComponent* ResolveCombo() const;

	/**
	 * Applies the attack's movement scaling, and puts it back on end.
	 *
	 * Goes through UARPGLocomotionComponent where the character has one. Writing
	 * MaxWalkSpeed directly -- which this used to do -- is silently undone on the
	 * very next frame, because that component reapplies its own tier every tick.
	 * A rooted attack therefore ran at full speed on exactly the characters that
	 * matter most. The direct write survives only as the fallback for a character
	 * with no locomotion component, which is every NPC.
	 */
	void ApplyMovementScaling();

	/** Stamps the swing's payload onto the hitbox and arms it. */
	void ArmHitbox(int32 WindowIndex, bool bLandingWindow);
	void DisarmHitbox();

	/** Puts back anything the attack changed on the character. */
	void RestoreCharacter();

	/** Starts the montage, applying whatever charge or channel setup it needs. */
	void PlayAttackMontage();

	/** Length of a named section, or 0 when the montage has no such section. */
	float GetSectionLength(FName SectionName) const;

	/** Breaks the Active loop and heads for Recovery, ending the montage if absent. */
	void LeaveChannelLoop();

	UPROPERTY(Transient)
	TObjectPtr<const UARPGAttackDefinition> CurrentAttack;

	UPROPERTY(Transient)
	TObjectPtr<UARPGHitboxComponent> ArmedHitbox;

	/** Restored on end, so a rooted attack does not leave the character slowed. */
	float CachedMaxWalkSpeed = 0.f;
	bool bAppliedSpeedFactor = false;
	bool bScaledViaLocomotion = false;
	bool bGrantedHyperarmor = false;

	/**
	 * The hitbox's own damage type, put back when the attack ends.
	 *
	 * An attack that overrides the type used to leave the override in place --
	 * there was no else branch -- so the first fire swing in a tree converted the
	 * weapon to fire permanently.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UARPGDamageTypeAsset> CachedHitboxDamageType;
	bool bOverrodeDamageType = false;

	/** Same snapshot-and-restore for HitboxRadiusScale, and for the same reason. */
	float CachedHitboxRadius = 0.f;
	bool bOverrodeHitboxRadius = false;

	/**
	 * Beats already played when this attack began, read once from the combo
	 * component. See UARPGComboComponent::GetChainDepth.
	 *
	 * Latched at activation rather than read at each window: the chain is what
	 * the swing was BOUGHT with, and a component whose state moved on mid-swing
	 * must not retroactively change what the swing is worth.
	 */
	int32 ChainDepth = 0;

	/** Guards against the combo window notify firing twice on a looping montage. */
	bool bComboWindowOpened = false;

	/** OnBlendOut and OnCompleted both fire on a normal end; only the first counts. */
	bool bMontageFinishHandled = false;

	/**
	 * The hitbox-fallback warning is worth saying once per swing, not once per
	 * window. Mutable because ResolveHitbox is const and this is pure bookkeeping.
	 */
	mutable bool bWarnedHitboxFallback = false;

	/**
	 * How far the charge got, 0-1. Drives GetChargedMotionValue when the hitbox
	 * arms, so a released-early swing hits for less.
	 *
	 * Starts at 1 rather than 0: an attack that is not chargeable never receives
	 * a release event, and must not be scaled down for it.
	 */
	float ChargeFraction = 1.f;

	bool bCharging = false;
	bool bChanneling = false;
	bool bChannelLoopEnded = false;
};
