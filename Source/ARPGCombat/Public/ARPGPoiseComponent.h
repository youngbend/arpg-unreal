// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ARPGPoiseComponent.generated.h"

class UAbilitySystemComponent;

UENUM(BlueprintType)
enum class EARPGPoiseResult : uint8
{
	None,
	LightFlinch,
	HeavyFlinch,
	StanceBreak
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FARPGOnPoiseChanged, float, Current, float, Max);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnPoiseResult, EARPGPoiseResult, Result);

/**
 * The poise / stance meter. Port of Godot's PoiseComponent.
 *
 * NOT A SECOND HEALTH BAR. The meter starts empty and FILLS as hits land; a
 * stance break happens on reaching maximum, not on being emptied.
 *
 * Three payoffs, resolved in this order (heaviest first) on every contribution:
 *
 *   Stance Break   the meter tops out. Resets to zero and opens an immunity
 *                  window so the character cannot be chain-broken.
 *   Heavy Flinch   a SINGLE contribution >= HeavyFlinchThresholdPct of maximum.
 *   Light Flinch   a SINGLE contribution >= LightFlinchThresholdPct.
 *
 * Two details that are easy to get wrong and change the feel completely:
 *
 * THE FLINCH TIERS KEY ON ONE HIT, NOT THE TOTAL. Whether you stagger is about
 * how hard that particular blow was, independent of how worn down you already
 * are. Testing the running total instead would make every late-fight tap
 * stagger.
 *
 * NEITHER FLINCH RESETS THE METER. Accumulated poise keeps building toward a
 * break across the whole fight; flinching is not a release valve.
 *
 * HYPERARMOR COVERS THE FLINCHES ONLY. A hyperarmored attacker ploughs through
 * stagger but can still be stance-broken -- which is what stops hyperarmor moves
 * being an unanswerable defence. Note the ordering below: the break check
 * happens BEFORE the hyperarmor gate, deliberately.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCOMBAT_API UARPGPoiseComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGPoiseComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	// --- Tunables -------------------------------------------------------------

	/** Single-contribution flinch thresholds, as a fraction of maximum poise. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Poise",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float LightFlinchThresholdPct = 0.08f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Poise",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float HeavyFlinchThresholdPct = 0.15f;

	/** Seconds the meter holds after the last contribution before it starts draining. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Poise",
		meta = (ClampMin = "0.0"))
	float HoldTime = 2.5f;

	/** Poise drained per second once the hold window expires. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Poise",
		meta = (ClampMin = "0.0"))
	float DrainRate = 20.f;

	/** Seconds of immunity to a fresh break right after one triggers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Poise",
		meta = (ClampMin = "0.0"))
	float BreakImmunityTime = 1.5f;

	/** Poise dealt to the ATTACKER when this character perfect-parries. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Poise|Defensive",
		meta = (ClampMin = "0.0"))
	float ParryPoiseDamage = 15.f;

	/** Poise dealt to the ATTACKER when this character blocks without parrying. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Poise|Defensive",
		meta = (ClampMin = "0.0"))
	float BlockPoiseDamage = 4.f;

	// --- Runtime --------------------------------------------------------------

	/** Applies one contribution and resolves break/flinch. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Poise")
	EARPGPoiseResult ApplyPoiseDamage(float Amount);

	/** Empties the meter without firing a break -- death, respawn, manual cleanse. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Poise")
	void ResetPoise();

	UFUNCTION(BlueprintPure, Category = "ARPG|Poise")
	float GetCurrentPoise() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Poise")
	float GetMaxPoise() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Poise")
	bool IsBreakImmune() const { return BreakImmunityTimer > 0.f; }

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Poise")
	FARPGOnPoiseChanged OnPoiseChanged;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Poise")
	FARPGOnPoiseResult OnPoiseResult;

	/**
	 * Binds to the vital set's poise delegate if not already bound. Idempotent.
	 *
	 * Called from BeginPlay AND from tick, because BeginPlay is not a reliable
	 * moment to find the ability system component: a player's ASC lives on the
	 * PlayerState, which on a client may not have replicated in yet. Subscribing
	 * only once at BeginPlay would leave such a character permanently unable to
	 * accumulate poise, silently -- no error, stance simply never breaks.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Poise")
	void EnsureSubscribed();

private:
	UAbilitySystemComponent* GetASC() const;
	void SetPoise(float NewValue);
	void HandlePoiseDamageReceived(float Amount);

	/** Raises the matching Event.Poise.* so an ability can react to it. */
	void SendPoiseEvent(EARPGPoiseResult Result) const;

	UPROPERTY(Transient)
	mutable TObjectPtr<UAbilitySystemComponent> CachedASC;

	float HoldTimer = 0.f;
	float BreakImmunityTimer = 0.f;

	FDelegateHandle PoiseDamageHandle;
};
