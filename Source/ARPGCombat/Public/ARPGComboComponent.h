// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ARPGWeaponAttackTree.h"
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
class ARPGCOMBAT_API UARPGComboComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGComboComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Combo")
	TObjectPtr<UARPGWeaponAttackTree> AttackTree;

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
	bool IsCharging() const { return bCharging; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Combo")
	bool IsInCombo() const { return CurrentNode != nullptr; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Combo")
	UARPGComboAttackNode* GetCurrentNode() const { return CurrentNode; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Combo")
	float GetChargeElapsed() const { return ChargeElapsed; }

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

private:
	UAbilitySystemComponent* GetASC() const;
	UARPGComboAttackNode* ResolveNext(EARPGAttackInput Input) const;
	void StartAttack(EARPGAttackInput Input, bool bEmpowered);
	void ReleaseCharge();
	float NodeResetTimeout() const;
	bool TrySpendStamina(float Cost);

	UPROPERTY(Transient)
	mutable TObjectPtr<UAbilitySystemComponent> CachedASC;

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
	int32 ChargeInput = INDEX_NONE;
	bool bChargeEmpowered = false;

	bool bInputHeld[static_cast<uint8>(EARPGAttackInput::MAX)] = {};
	int32 LastInput = INDEX_NONE;
};
