// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "ARPGDischargeContext.h"
#include "ARPGMagicLoadout.h"
#include "ARPGMagicTypes.h"
#include "ARPGMagicComponent.generated.h"

class UAbilitySystemComponent;
class UARPGMagicCombinationTable;
class UARPGMagicElement;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnSelectionChanged, int32, ActiveMask);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnCombinationResolved, UARPGMagicElement*, Combination);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnSpellFailed, FGameplayTagContainer, ActiveElements);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FARPGOnElementBlocked, EARPGElementSlot, Slot, UARPGMagicElement*, Element);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnDischargeExecuted, const FARPGDischargeContext&, Context);

/**
 * The four authoring knobs that differ per discharge type.
 *
 * One struct in a map rather than sixteen parallel float properties. The Godot
 * version had exactly that, resolved through four parallel switch statements it
 * eventually collapsed into one -- keying by the enum removes the switch
 * entirely, and adding a discharge type stops meaning "find every switch".
 */
USTRUCT(BlueprintType)
struct ARPGMAGIC_API FARPGDischargeTypeSettings
{
	GENERATED_BODY()

	/** Mana at zero charge, before the element's usage rate is applied. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cost", meta = (ClampMin = "0.0"))
	float MinManaCost = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cost", meta = (ClampMin = "0.0"))
	float MaxManaCost = 0.f;

	/** Scales damage only -- never the cosmetic power window. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Damage", meta = (ClampMin = "0.0"))
	float DamageMultiplier = 1.f;

	/**
	 * Extra damage at full charge as a fraction: 0.5 means a full charge deals
	 * 50% more than none.
	 *
	 * A ratio rather than an absolute so mastery cannot stretch or compress it --
	 * mastery multiplies the charge-0 and charge-1 cases identically, and so only
	 * ever scales the whole curve.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Damage", meta = (ClampMin = "0.0"))
	float MaxChargeDamage = 1.f;
};

/**
 * Runtime magic manager. Port of Godot's MagicComponent.
 *
 * ELEMENT SELECTION IS ADDITIVE -- a "mixing bowl". Elements are added to the
 * active set and cannot be individually removed; clearing is wholesale. That is
 * a deliberate design constraint, not a missing feature: it makes the readied
 * combination a commitment the player has to spend mana to change, which is what
 * gives holding two elements its weight.
 *
 * WHAT THIS OWNS AND WHAT IT DOES NOT. This owns selection state, the
 * combination lookup, the complexity gate and the auto-ready sequence. It does
 * NOT cast: the discharge abilities do, reading BuildDischargeContext() for the
 * numbers. The split matters because a cast is predicted, interruptible and
 * animation-driven -- all things GAS does well -- while selection is persistent
 * state that outlives any one cast, which GAS does not model well at all. Same
 * division as UARPGComboComponent and the melee ability.
 *
 * SERVER-AUTHORITATIVE. Readying an element spends mana and gates what can be
 * cast, so the client asks and the server decides; the resulting mask replicates
 * back. Selection has a built-in interval, so the round trip is not felt the way
 * it would be on a per-frame input.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGMAGIC_API UARPGMagicComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGMagicComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// --- Configuration --------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Magic")
	TObjectPtr<UARPGMagicLoadout> Loadout;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Magic")
	TObjectPtr<UARPGMagicCombinationTable> CombinationTable;

	/** Every primitive the caster has acquired. Gates what a loadout page may hold. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|Magic")
	TArray<TObjectPtr<UARPGMagicElement>> UnlockedElements;

	/** Minimum seconds between selections, and the auto-ready step interval. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Magic",
		meta = (ClampMin = "0.0"))
	float SelectionInterval = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|Magic|Cost")
	TMap<EARPGDischargeType, FARPGDischargeTypeSettings> DischargeSettings;

	/** Flat, unchargeable costs before the element's usage rate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Magic|Cost",
		meta = (ClampMin = "0.0"))
	float ImbueManaCost = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Magic|Cost",
		meta = (ClampMin = "0.0"))
	float DodgeManaCost = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Magic|Damage",
		meta = (ClampMin = "0.0"))
	float ImbueDamageMultiplier = 1.f;

	/** The cosmetic power window, shared by every chargeable type. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Magic|Power",
		meta = (ClampMin = "0.0"))
	float MinPower = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Magic|Power",
		meta = (ClampMin = "0.0"))
	float MaxPower = 2.f;

	/**
	 * Added to both ends of the power window at full mastery, scaled by the
	 * caster's proficiency. Moves the LOOK of a cast only -- never its damage.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Magic|Power",
		meta = (ClampMin = "0.0"))
	float MasteryPowerBonus = 0.f;

	// --- Selection ------------------------------------------------------------

	/** Ready the element in this slot. Server-authoritative; safe to call from a client. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	void ToggleElement(EARPGElementSlot Slot);

	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	void ClearSelection();

	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	void SetCurrentPage(int32 Page);

	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	int32 GetCurrentPage() const { return CurrentPage; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	int32 GetActiveMask() const { return ActiveMask; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	bool IsSlotActive(EARPGElementSlot Slot) const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	int32 GetActiveCount() const { return UARPGMagicLoadout::ActiveCount(ActiveMask); }

	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	bool HasActiveElements() const { return ActiveMask != 0; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	TArray<UARPGMagicElement*> GetActiveElements() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	FGameplayTagContainer GetActiveElementTags() const;

	/** The combination result if the active set matches a hand-scope row, else null. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	UARPGMagicElement* GetResolvedCombination() const;

	/**
	 * The element that drives everything: the combination when one resolved,
	 * otherwise the single active primitive. Null when nothing is readied.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	UARPGMagicElement* GetDisplayElement() const;

	/**
	 * Whether the readied set can actually be cast. Two or more elements with no
	 * matching row is a failed spell, not a multi-element one.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	bool CanDischarge() const;

	// --- Consumption ----------------------------------------------------------

	/**
	 * Spend the readied element on one imbued weapon strike. Returns what to read
	 * damage type and status from, or null when nothing was readied or the mana
	 * was not there. Clears the selection on success.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	UARPGMagicElement* ConsumeForImbue();

	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	UARPGMagicElement* ConsumeForDodge();

	/**
	 * Imbue damage for one swing. No charge term -- imbue is not chargeable,
	 * which is also why it pays a flat cost.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	float GetImbueDamage(const UARPGMagicElement* Element, float MotionValue) const;

	/** Poise counterpart. No charge and no mastery -- see UARPGMagicElement. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	float GetImbuePoiseDamage(const UARPGMagicElement* Element, float MotionValue) const;

	// --- Discharge ------------------------------------------------------------

	/**
	 * The full cast snapshot for the readied element. Called by the discharge
	 * abilities; does not spend anything or clear the selection.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	FARPGDischargeContext BuildDischargeContext(EARPGDischargeType Type, FVector Origin,
		FVector Direction, float Charge, bool bForcedRelease = false) const;

	/**
	 * Called by a discharge ability once the cast has actually happened: raises
	 * OnDischargeExecuted, clears the selection, and remembers it for auto-ready.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	void NotifyDischarged(const FARPGDischargeContext& Context);

	/** Total mana a cast of this type at this charge would cost. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	float GetDischargeManaCost(EARPGDischargeType Type, float Charge) const;

	/** Settings for a type, or sane defaults when none were authored. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	FARPGDischargeTypeSettings GetDischargeSettings(EARPGDischargeType Type) const;

	/**
	 * Mana on hand, or effectively unlimited when there is no ability system to
	 * hold a pool.
	 *
	 * The unlimited case is not a fallback so much as the same rule TrySpendMana
	 * follows: a caster with no resource system is not a caster with no mana, it
	 * is one the resource system does not apply to. The two MUST agree -- if this
	 * reported 0 while spending succeeded, the charge task would force a release
	 * on the first tick of every cast in a fixture with no ASC.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	float GetAvailableMana() const;

	/**
	 * Public only for UARPGAbilityTask_ChargeDischarge, which pays for a charge
	 * incrementally as it fills and so cannot go through a one-shot cost.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	bool TrySpendManaForCharge(float Cost) { return TrySpendMana(Cost); }

	/**
	 * The rate driving cost: the combination's own when one resolved, otherwise
	 * the sum of the active primitives'. A combination REPLACES rather than sums,
	 * so a combo spell is tuned on its own terms.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	float GetUsageRate() const;

	// --- Auto-ready -----------------------------------------------------------

	/**
	 * Re-ready the last combination that was cast, one slot at a time. Called
	 * when the cast input fires with nothing readied.
	 *
	 * Each step pays the element's normal activation cost, so repeating a
	 * combination is convenient but not free.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	void BeginAutoReady();

	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	void CancelAutoReady();

	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	bool IsAutoReadyPending() const { return AutoReadyTimer >= 0.f; }

	// --- Events ---------------------------------------------------------------

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Magic")
	FARPGOnSelectionChanged OnSelectionChanged;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Magic")
	FARPGOnCombinationResolved OnCombinationResolved;

	/** Two or more elements readied with no matching combination. */
	UPROPERTY(BlueprintAssignable, Category = "ARPG|Magic")
	FARPGOnSpellFailed OnSpellFailed;

	/** Refused by the complexity gate -- the caster is not skilled enough yet. */
	UPROPERTY(BlueprintAssignable, Category = "ARPG|Magic")
	FARPGOnElementBlocked OnElementGateBlocked;

	/** Refused for mana, or because the active limit is already reached. */
	UPROPERTY(BlueprintAssignable, Category = "ARPG|Magic")
	FARPGOnElementBlocked OnElementActivationFailed;

	/**
	 * A cast actually happened. Fires on every discharge whether or not the
	 * resulting effect lands on anything, which is what the magic progression
	 * tracker keys off -- see UARPGMagicProgressionTracker.
	 */
	UPROPERTY(BlueprintAssignable, Category = "ARPG|Magic")
	FARPGOnDischargeExecuted OnDischargeExecuted;

protected:
	UFUNCTION(Server, Reliable)
	void ServerToggleElement(EARPGElementSlot Slot);

	UFUNCTION(Server, Reliable)
	void ServerClearSelection();

	UFUNCTION(Server, Reliable)
	void ServerSetCurrentPage(int32 Page);

	UFUNCTION()
	void OnRep_ActiveMask();

private:
	UAbilitySystemComponent* GetASC() const;

	/** Mana spend, returning false without spending when it is unaffordable. */
	bool TrySpendMana(float Cost);

	/**
	 * Shared body of player-driven and auto-ready selection. bFromPlayer is what
	 * distinguishes them: a manual press interrupts an in-progress auto-ready,
	 * and the auto-ready stepper obviously must not interrupt itself.
	 */
	void ActivateSlot(EARPGElementSlot Slot, bool bFromPlayer);

	/**
	 * A candidate may join the readied set only if its mastery covers every
	 * already-readied element's complexity, and each of theirs covers its. Always
	 * true with no progression implementation present.
	 */
	bool PassesComplexityGate(const UARPGMagicElement* Candidate) const;

	float GetEffectiveLevel(const UARPGMagicElement* Element) const;
	float GetMasteryDamageMultiplier(const UARPGMagicElement* Element) const;

	/**
	 * Whoever answers for this caster's mastery: a component implementing
	 * IARPGMagicProgression, or the owning actor itself.
	 *
	 * Components are checked FIRST because the real tracker is one; the actor
	 * fallback exists so a simple test caster can answer without hosting a
	 * component. Null means no progression system applies -- gating off, and
	 * multipliers at 1.
	 */
	UObject* GetProgressionProvider() const;

	/** Outgoing damage amp, so one Weakened weakens spells and swings alike. */
	float GetDamageAmpMultiplier() const;

	void AutoReadyStep();

	UPROPERTY(Transient)
	mutable TObjectPtr<UAbilitySystemComponent> CachedASC;

	UPROPERTY(ReplicatedUsing = OnRep_ActiveMask)
	int32 ActiveMask = 0;

	UPROPERTY(Replicated)
	int32 CurrentPage = 0;

	float SelectionCooldown = 0.f;

	/**
	 * The full mask of the last successful cast, kept across the clear so
	 * auto-ready has something to restore.
	 */
	int32 LastDischargedMask = 0;

	/** Slots still waiting to be re-readied this sequence. */
	int32 AutoReadyMask = 0;

	/** Counts down to the next auto-ready step. Negative means inactive. */
	float AutoReadyTimer = -1.f;
};
