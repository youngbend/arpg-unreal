// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGGameplayComponentBase.h"
#include "ARPGWeaponAttackTree.h"
#include "GameplayTagContainer.h"
#include "ARPGComboComponent.generated.h"

class UAbilitySystemComponent;
class UARPGAttackDefinition;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FARPGOnAttackStarted,
	UARPGComboAttackNode*, Node, bool, bEmpowered);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FARPGOnComboReset);

/**
 * Drives branching combo execution. Port of Godot's ComboComponent.
 *
 * DELIBERATELY NOT A GAMEPLAY ABILITY. The obvious GAS shape is one ability that
 * owns the whole chain, but the state here -- tree position, a buffered press
 * with its own expiry, a reset timer, a finisher lockout, per-input held flags,
 * a channel that may already have been released -- outlives any single
 * activation and has to survive an attack being cancelled mid-swing. Expressing
 * that as an ability graph reimplements this component inside GAS with worse
 * ergonomics.
 *
 * So this owns the state and DRIVES abilities: it raises Event.Attack.Begin
 * carrying the resolved attack, and UGA_MeleeAttack does the montage, the cost
 * and the tags. What GAS is genuinely better at -- cancelling on dodge, blocking
 * while flinching, granting hyperarmor -- becomes declarative tag setup on that
 * ability instead of the manual attack_locked / cancel_attack() plumbing the
 * Godot version carried.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCOMBAT_API UARPGComboComponent : public UARPGGameplayComponentBase
{
	GENERATED_BODY()

public:
	UARPGComboComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * The equipped weapon's moveset. WRITTEN ONLY BY UARPGWeaponComponent.
	 *
	 * Left null while nothing is equipped, which is correct: an unarmed
	 * character has no weapon moveset. FallbackAttackTree is what answers in
	 * that case -- see GetActiveTree.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Combo")
	TObjectPtr<UARPGWeaponAttackTree> AttackTree;

	/**
	 * The moveset used when no weapon supplies one -- bare hands, or a character
	 * with no weapon component at all.
	 *
	 * A SECOND FIELD rather than a default written into AttackTree, because two
	 * writers of one field is what this replaces. The character used to fill
	 * AttackTree from its own soft reference whenever it found it empty, while
	 * the weapon component cleared it on unequip; which of the two won depended
	 * on whether PossessedBy ran before or after the weapon component's
	 * BeginPlay, so unequipping a weapon either left the character unarmed or
	 * silently gave them the character's default sword moveset.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Combo")
	TObjectPtr<UARPGWeaponAttackTree> FallbackAttackTree;

	/** The tree actually driving resolution: the weapon's, or the fallback. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Combo")
	UARPGWeaponAttackTree* GetActiveTree() const
	{
		return AttackTree ? AttackTree.Get() : FallbackAttackTree.Get();
	}

	/**
	 * How long a press stays valid, measured FROM THE PRESS -- not from when the
	 * attack started.
	 *
	 * That distinction is the whole design. A short attack buffers across its
	 * entire duration because its cancel point is never further away than this;
	 * a long one only accepts presses in its tail. Measuring from attack start
	 * instead would commit the player to their next action before they could see
	 * the current swing resolve.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Combo",
		meta = (ClampMin = "0.0"))
	float BufferWindow = 0.25f;

	// --- Input ----------------------------------------------------------------

	/** An attack button was pressed. bEmpowered is true straight after a successful parry. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Combo")
	void ReceiveInput(EARPGAttackInput Input, bool bEmpowered = false);

	UFUNCTION(BlueprintCallable, Category = "ARPG|Combo")
	void ReceiveInputReleased(EARPGAttackInput Input);

	/** Called when the montage reaches its cancel window. Fires a buffered press, or auto-chains. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Combo")
	void NotifyAttackFinished();

	/**
	 * The channel's windup finished, so the loop may begin. Called by the ability
	 * once the montage's Windup section has played out.
	 *
	 * Returns false, and ends the channel, when the input was already let go
	 * during the windup -- a tap on a channel attack should play the wind-up and
	 * stop, not commit to a loop nobody asked for.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Combo")
	bool NotifyChannelWindupFinished();

	/**
	 * Drains one frame of channel stamina. Returns false once the channel should
	 * stop -- exhausted or released.
	 *
	 * Called from this component's own tick while the loop is running, and safe
	 * to call at any other time: it is a no-op unless bChannelLooping, which is
	 * only true between the wind-up finishing and the loop breaking. That is what
	 * keeps the drain off the wind-up and the recovery, where the player is not
	 * yet (or no longer) getting anything for the stamina.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Combo")
	bool TickChannelLoop(float DeltaTime);

	/** The channel's recovery finished. Closes it out like NotifyAttackFinished. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Combo")
	void NotifyChannelEnded();

	/** Interrupt immediately -- dodge, hitstun, death. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Combo")
	void CancelAttack();

	/** Return to root without attacking. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Combo")
	void ResetCombo();

	/** Refresh the reset timer -- e.g. the player readied an element mid-combo. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Combo")
	void KeepAlive();

	// --- State ----------------------------------------------------------------

	/**
	 * While true no NEW attack may begin, but one already in progress is
	 * untouched. Driven by flinch recovery.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ARPG|Combo")
	bool bAttackLocked = false;

	UFUNCTION(BlueprintPure, Category = "ARPG|Combo")
	bool IsAttacking() const { return bAttacking || bCharging; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Combo")
	bool IsChanneling() const { return bChanneling; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Combo")
	bool IsChannelLooping() const { return bChannelLooping; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Combo")
	bool IsCharging() const { return bCharging; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Combo")
	bool IsInCombo() const { return CurrentNode != nullptr; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Combo")
	UARPGComboAttackNode* GetCurrentNode() const { return CurrentNode; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Combo")
	float GetChargeElapsed() const { return ChargeElapsed; }

	/**
	 * How far the charge got, 0-1. Latched at release and kept until the next
	 * charge begins, because the swing it scales outlives the charge phase.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Combo")
	float GetChargeFraction() const { return ChargeFraction; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Combo")
	float GetRootLockoutRemaining() const { return RootLockoutTimer; }

	/**
	 * Increments every time a new beat actually begins -- a fresh press, a
	 * buffered press, or a continuous-hold continuation all count.
	 *
	 * IsAttacking() alone cannot distinguish "still the same beat" from "a new
	 * one already started": NotifyAttackFinished clears the flag and, when
	 * chaining, sets it again inside the same call. A poller watching for a
	 * false->true transition never observes it. Comparing this counter does.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Combo")
	int32 GetAttackSequenceNumber() const { return AttackSequenceNumber; }

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Combo")
	FARPGOnAttackStarted OnAttackStarted;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Combo")
	FARPGOnAttackStarted OnChargeStarted;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Combo")
	FARPGOnComboReset OnComboReset;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Combo")
	FARPGOnAttackStarted OnChannelLoopStarted;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Combo")
	FARPGOnAttackStarted OnChannelEnded;

private:
	/**
	 * Which node an input resolves to, and whether that meant falling back to
	 * root.
	 *
	 * THE ONE COPY. This used to exist twice with different rules: ReceiveInput
	 * peeked with a version that ignored the parry follow-up branch, while
	 * StartAttack resolved with one that checked it first. An empowered press at
	 * root therefore peeked the root node -- deciding from it whether the attack
	 * was a charge or a channel -- and then executed the follow-up instead. Where
	 * the two nodes disagreed on bChargeable or bChannel, the component took the
	 * wrong branch entirely and a channel never got its flags set.
	 *
	 * @param bOutFallsBackToRoot  true when neither a follow-up nor a parry
	 *                             follow-up matched, which is what the finisher
	 *                             lockout gates on.
	 */
	UARPGComboAttackNode* ResolveNext(EARPGAttackInput Input, bool bEmpowered,
		bool& bOutFallsBackToRoot) const;

	void StartAttack(EARPGAttackInput Input, bool bEmpowered);

	/** Ticking is only needed while a timer, a buffer, a charge or a channel is live. */
	void RefreshTickState();
	void ReleaseCharge();
	void StartChannel(UARPGComboAttackNode* Node, int32 InputIndex, bool bEmpowered);
	void EndChannelLoop();

	/** Raises Event.Attack.Begin for the current node, handing off to the ability. */
	void SendAttackBeginEvent(bool bEmpowered);

	/** Raises a bare Event.Attack.* at the running ability. */
	void SendAbilityEvent(const FGameplayTag& EventTag, float Magnitude);
	float NodeResetTimeout() const;
	bool TrySpendStamina(float Cost);

	UPROPERTY(Transient)
	TObjectPtr<UARPGComboAttackNode> CurrentNode;

	bool bAttacking = false;
	int32 AttackSequenceNumber = 0;

	int32 BufferedInput = INDEX_NONE;
	bool bBufferedEmpowered = false;
	float BufferTimer = 0.f;

	float ResetTimer = 0.f;
	float RootLockoutTimer = 0.f;

	bool bCharging = false;
	float ChargeElapsed = 0.f;
	float ChargeFraction = 0.f;
	int32 ChargeInput = INDEX_NONE;
	bool bChargeEmpowered = false;

	bool bChanneling = false;      // true across both the windup and the loop
	bool bChannelLooping = false;  // true once the loop itself has begun
	int32 ChannelInput = INDEX_NONE;
	bool bChannelEmpowered = false;
	bool bChannelReleaseRequested = false;

	bool bInputHeld[static_cast<uint8>(EARPGAttackInput::MAX)] = {};
	int32 LastInput = INDEX_NONE;
};
