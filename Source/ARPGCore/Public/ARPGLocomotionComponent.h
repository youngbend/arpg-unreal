// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGGameplayComponentBase.h"
#include "ARPGLocomotionComponent.generated.h"

class UAbilitySystemComponent;
class UCharacterMovementComponent;

UENUM(BlueprintType)
enum class EARPGSpeedTier : uint8
{
	/** Forced, not chosen: something in hand says you may not run with it. */
	Walk = 0,
	Run = 1,
	Sprint = 2
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnSpeedTierChanged, EARPGSpeedTier, Tier);

/**
 * Speed tiers, sprint and the stamina that pays for it. Port of the movement
 * policy half of Godot's PlayerCharacter.
 *
 * Writes MaxWalkSpeed on the owner's character movement component and nothing
 * else -- the actual movement stays with the engine. What ports is the POLICY:
 * which of three speeds applies right now, and what is allowed to take it away.
 *
 * Three things lower the tier, and they are not the same kind of thing:
 *
 *   - **A readied element forces a walk.** Carrying live magic in an open hand
 *     is the cost of having it ready, and it is the clearest tell an opponent
 *     gets that a discharge is coming. Set by the owner via SetWalkForced --
 *     this module sits below the magic module and cannot ask directly, which is
 *     the right way round: the rule is "something says walk", not "magic says
 *     walk", so a heavy chest or a carried objective can use it later.
 *   - **An attack scales the whole tier** through SetAttackMovement, which is
 *     how an attack that steps forward differs from one that roots you.
 *   - **Stamina runs out**, and the sprint simply ends. Not blocked from
 *     restarting, not put on cooldown: the player can pull the stick again and
 *     get a step out of whatever regenerated.
 *
 * Sprint is a TOGGLE rather than a hold, which is worth preserving deliberately.
 * The scheme already asks the left hand to hold LT and the right to hold RT and
 * a face button; a sprint that also had to be held would collide with all of
 * it. It drops itself on the conditions above and on the stick returning to
 * centre, so it never survives into a state the player did not ask for.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCORE_API UARPGLocomotionComponent : public UARPGGameplayComponentBase
{
	GENERATED_BODY()

public:
	UARPGLocomotionComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	// --- Sprint ----------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "ARPG|Locomotion")
	void SetSprinting(bool bNewSprinting);

	UFUNCTION(BlueprintCallable, Category = "ARPG|Locomotion")
	void ToggleSprint() { SetSprinting(!bSprinting); }

	UFUNCTION(BlueprintPure, Category = "ARPG|Locomotion")
	bool IsSprinting() const { return bSprinting; }

	/**
	 * How hard the movement stick is deflected, 0..1.
	 *
	 * Fed in by the owner every frame it moves. Centring the stick ends a
	 * sprint -- letting go and pushing again is how a player steers out of one,
	 * and a sprint that survived the release would resume at full speed in a new
	 * direction the moment they nudged the stick.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Locomotion")
	void SetMoveMagnitude(float Magnitude);

	/**
	 * True while the movement input should be treated as fully deflected.
	 *
	 * Sprint alone ignores stick magnitude: a half-pressed sprint is a run, and
	 * the tier the player explicitly asked for should not depend on how far
	 * their thumb happens to be. Walk and run both scale normally, which the
	 * engine's own movement component already does for free.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Locomotion")
	bool ShouldIgnoreInputMagnitude() const { return bSprinting; }

	// --- Tier ------------------------------------------------------------------

	/** Set while a readied element (or anything else) forbids running. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Locomotion")
	void SetWalkForced(bool bForced);

	UFUNCTION(BlueprintPure, Category = "ARPG|Locomotion")
	bool IsWalkForced() const { return bWalkForced; }

	/**
	 * Scales movement for the duration of an attack.
	 *
	 * Factor 0 roots the character; 1 leaves them at full speed. bAllowSprint is
	 * separate rather than implied by the factor because a slow attack that may
	 * still be sprint-cancelled and a fast one that may not are both real.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Locomotion")
	void SetAttackMovement(float Factor, bool bAllowSprint);

	/** Back to unscaled movement with sprinting allowed again. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Locomotion")
	void ClearAttackMovement() { SetAttackMovement(1.f, true); }

	UFUNCTION(BlueprintPure, Category = "ARPG|Locomotion")
	EARPGSpeedTier GetSpeedTier() const;

	/** The speed the movement component is being driven to, attack scaling included. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Locomotion")
	float GetCurrentSpeed() const;

	/** Pushes GetCurrentSpeed() onto the owner's movement component. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Locomotion")
	void ApplySpeed();

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Locomotion")
	FARPGOnSpeedTierChanged OnSpeedTierChanged;

	// --- Config ----------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Locomotion", meta = (ClampMin = "0.0"))
	float WalkSpeed = 200.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Locomotion", meta = (ClampMin = "0.0"))
	float RunSpeed = 500.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Locomotion", meta = (ClampMin = "0.0"))
	float SprintSpeed = 750.f;

	/** Stamina per second while sprinting. Zero makes sprinting free. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Locomotion", meta = (ClampMin = "0.0"))
	float SprintStaminaDrain = 12.f;

protected:
	UCharacterMovementComponent* GetMovement() const;

	/**
	 * Spends stamina, or returns false having spent nothing.
	 *
	 * Returns TRUE when there is no ability system to spend from, matching the
	 * combo component: a character with no attributes should move, not be pinned
	 * in place by a resource it does not have.
	 */
	bool TrySpendStamina(float Cost);

	/** Broadcasts OnSpeedTierChanged if the tier moved, then reapplies the speed. */
	void RefreshTier();

	/** Follows the sprint outside the editor -- nothing else here needs a tick. */
	void RefreshTickState();

private:
	bool bSprinting = false;
	bool bWalkForced = false;
	bool bSprintAllowed = true;
	float AttackSpeedFactor = 1.f;
	float MoveMagnitude = 0.f;

	EARPGSpeedTier LastBroadcastTier = EARPGSpeedTier::Run;
};
