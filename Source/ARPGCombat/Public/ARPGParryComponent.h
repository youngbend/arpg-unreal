// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ARPGParryComponent.generated.h"

class UAbilitySystemComponent;

UENUM(BlueprintType)
enum class EARPGInterceptResult : uint8
{
	None,
	Blocked,
	Parried
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FARPGOnBlockStateChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnIntercept, EARPGInterceptResult, Result);

/**
 * The block / parry state machine. Port of Godot's ParryComponent.
 *
 * Holding guard runs through three phases:
 *
 *   [0, BlendInTime)                            raising the guard. NOT yet
 *                                               parrying -- a hit here is not
 *                                               even blocked.
 *   [BlendInTime, +ParryWindow)                 perfect parry window.
 *   thereafter                                  ordinary block, no parry
 *                                               potential until released and
 *                                               re-pressed.
 *
 * THE BLEND-IN IS THE COST OF THE PARRY. Without it, guard could be raised on
 * reaction to any hit and parrying would carry no risk. It is also why
 * BTAttemptParry (phase 8) has to raise guard during the attacker's WINDUP
 * rather than reacting to the swing: the geometry gives less warning than the
 * blend-in needs.
 *
 * ParryCooldown stops mashing the guard key producing a continuous parry window.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCOMBAT_API UARPGParryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGParryComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	// --- Timing (overridden per weapon from BlockDefinition) ------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Parry",
		meta = (ClampMin = "0.0"))
	float BlendInTime = 0.10f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Parry",
		meta = (ClampMin = "0.05"))
	float ParryWindow = 0.15f;

	/** Blocks a fresh parry window from opening if guard is re-pressed too quickly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Parry",
		meta = (ClampMin = "0.0"))
	float ParryCooldown = 0.5f;

	// --- Mitigation -----------------------------------------------------------

	/** Fraction of damage stopped by an ordinary block. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Parry",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BlockDamageReduction = 0.50f;

	/** Fraction stopped by a perfect parry. 0.95 means 5% still lands. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Parry",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ParryDamageReduction = 0.95f;

	// --- Cost -----------------------------------------------------------------

	/** Stamina per point of damage an ordinary block actually stopped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Parry",
		meta = (ClampMin = "0.0"))
	float BlockStaminaMultiplier = 0.20f;

	/** Flat stamina for a successful parry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Parry",
		meta = (ClampMin = "0.0"))
	float ParryStaminaCost = 5.f;

	/** How long the empowered state lasts after a parry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Parry",
		meta = (ClampMin = "0.0"))
	float EmpoweredDuration = 5.f;

	// --- Runtime --------------------------------------------------------------

	/** Raise guard. No-op if already blocking. The caller must verify no attack is active. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Parry")
	void BeginBlock();

	UFUNCTION(BlueprintCallable, Category = "ARPG|Parry")
	void EndBlock();

	/**
	 * Resolve an incoming hit against the guard. Called from the damage
	 * execution, server-side.
	 *
	 * Mutating: a parry consumes the window and grants empowered.
	 */
	EARPGInterceptResult TryIntercept();

	/** Consume the empowered flag. Returns true once. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Parry")
	bool ConsumeEmpowered();

	UFUNCTION(BlueprintPure, Category = "ARPG|Parry")
	bool IsBlocking() const { return bBlocking; }

	/** True only during the parry window -- after blend-in, before it lapses. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Parry")
	bool IsParryActive() const { return bBlocking && BlendInTimer <= 0.f && ParryTimer > 0.f; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Parry")
	bool IsEmpowered() const { return bEmpowered; }

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Parry")
	FARPGOnBlockStateChanged OnBlockStarted;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Parry")
	FARPGOnBlockStateChanged OnBlockEnded;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Parry")
	FARPGOnIntercept OnIntercepted;

private:
	UAbilitySystemComponent* GetASC() const;

	UPROPERTY(Transient)
	mutable TObjectPtr<UAbilitySystemComponent> CachedASC;

	bool bBlocking = false;
	bool bEmpowered = false;
	float BlendInTimer = 0.f;
	float ParryTimer = 0.f;
	float ParryCooldownTimer = 0.f;
	float EmpoweredTimer = 0.f;
};
