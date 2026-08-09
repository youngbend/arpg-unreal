// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ARPGModalInputComponent.generated.h"

class UInputAction;
class UEnhancedInputComponent;

/**
 * The four face buttons, in the order the magic loadout pages its slots.
 *
 * Deliberately matches EARPGElementSlot's numbering (North/West/South/East =
 * 0/1/2/3) so LT + face maps onto a loadout slot by cast rather than by a table
 * that could drift out of step with it.
 */
UENUM(BlueprintType)
enum class EARPGInputFace : uint8
{
	/** Y -- heavy attack, or element slot 0. */
	North = 0,
	/** X -- light attack, or element slot 1. */
	West = 1,
	/** A -- jump, or element slot 2. */
	South = 2,
	/** B -- dodge, or element slot 3. */
	East = 3,

	/** No face button; the sentinel for "nothing is charging". */
	None = 4 UMETA(Hidden)
};

UENUM(BlueprintType)
enum class EARPGInputDPad : uint8
{
	Up = 0,
	Down = 1,
	Left = 2,
	Right = 3
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FARPGOnModalInput);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnModalInputSlot, EARPGInputFace, Slot);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FARPGOnModalInputCharge, EARPGInputFace, Slot, float, Charge);

/**
 * The modal control scheme. Port of Godot's InputComponent.
 *
 * Four face buttons carry three different meanings depending on which trigger
 * is held, which is what lets a controller reach the whole kit without a radial
 * menu or a modifier chord the player has to memorise:
 *
 *   - **Nothing held** -- Y heavy, X light, A jump, B dodge, RB special.
 *   - **LT held** -- a face press READIES that element slot, instantly, on
 *     press. D-pad Down discards what is readied; Left/Right page the loadout.
 *   - **RT held** -- a face press starts CHARGING that slot; releasing the face
 *     button discharges it at whatever charge it reached.
 *
 * Two rules in here look like oversights and are neither:
 *
 *   - **LT + D-pad Up is deliberately swallowed.** Nothing is bound to it, but
 *     it still consumes the press rather than falling through to the quick-slot
 *     bar -- otherwise a mistimed press while readying an element drinks a
 *     potion, which is the single most expensive misfire in the scheme.
 *   - **Releasing RT does NOT cancel a charge in progress.** RT's whole job is
 *     to START the charge; from then on the charge belongs to its own face
 *     button and ends when THAT button comes up. Requiring the player to keep
 *     RT down as well makes a charged discharge a two-finger hold for no gain.
 *
 * This class is a pure translator: raw actions in, semantic events out, no
 * gameplay knowledge whatsoever. Everything that decides what an event MEANS --
 * whether a dodge is elemental, whether an attack imbues, whether the character
 * is even alive -- lives on the receiving side. That separation is what keeps
 * the modal state machine testable without a pawn, a controller or an ability
 * system, and it is the reason the Godot original was worth keeping intact.
 *
 * Binding is optional. Call BindActions() from SetupPlayerInputComponent to
 * drive it from Enhanced Input, or call the press and release entry points
 * directly -- from a test, a replay, or an on-screen touch layout.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCORE_API UARPGModalInputComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGModalInputComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	// --- Enhanced Input --------------------------------------------------------

	/**
	 * Binds every configured action on the given component.
	 *
	 * The two modifiers are bound FIRST, on purpose. Enhanced Input dispatches a
	 * frame's bindings in the order they were added, so binding LT and RT ahead
	 * of the face buttons means a trigger and a face button pressed on the same
	 * frame are seen in that order -- the modifier is already up to date by the
	 * time the face press asks about it. PollModifiers() below covers the case
	 * where that is not enough.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Input")
	void BindActions(UEnhancedInputComponent* Input);

	// --- Modal state machine ---------------------------------------------------
	//
	// The whole scheme, reachable without Enhanced Input. Order matters between
	// the modifier setters and the press handlers; see BindActions.

	UFUNCTION(BlueprintCallable, Category = "ARPG|Input")
	void SetMagicModifier(bool bHeld);

	UFUNCTION(BlueprintCallable, Category = "ARPG|Input")
	void SetDischargeModifier(bool bHeld);

	UFUNCTION(BlueprintCallable, Category = "ARPG|Input")
	void PressFace(EARPGInputFace Slot);

	UFUNCTION(BlueprintCallable, Category = "ARPG|Input")
	void ReleaseFace(EARPGInputFace Slot);

	UFUNCTION(BlueprintCallable, Category = "ARPG|Input")
	void PressSpecial();

	UFUNCTION(BlueprintCallable, Category = "ARPG|Input")
	void ReleaseSpecial();

	UFUNCTION(BlueprintCallable, Category = "ARPG|Input")
	void PressParry();

	UFUNCTION(BlueprintCallable, Category = "ARPG|Input")
	void ReleaseParry();

	UFUNCTION(BlueprintCallable, Category = "ARPG|Input")
	void PressDPad(EARPGInputDPad Direction);

	/**
	 * Drops a charge in progress without discharging it.
	 *
	 * The Godot original declared this signal and never emitted it, so a charge
	 * begun before dying survived into the respawn and fired on the first face
	 * release afterwards. The owner calls this on death, on a stance break, or
	 * anywhere else a held input should stop meaning what it meant.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Input")
	void CancelCharge();

	// --- State -----------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "ARPG|Input")
	bool IsMagicModifierHeld() const { return bMagicModifierHeld; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Input")
	bool IsDischargeModifierHeld() const { return bDischargeModifierHeld; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Input")
	bool IsParryHeld() const { return bParryHeld; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Input")
	bool IsCharging() const { return ChargingSlot != EARPGInputFace::None; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Input")
	EARPGInputFace GetChargingSlot() const { return ChargingSlot; }

	/** Charge accumulated so far, normalised to 0..1 by MaxChargeTime. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Input")
	float GetChargeFraction() const;

	// --- Events ----------------------------------------------------------------

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnLightAttack;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnLightAttackReleased;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnHeavyAttack;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnHeavyAttackReleased;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnSpecialAttack;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnSpecialAttackReleased;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnJump;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnDodge;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnParryPressed;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnParryReleased;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnSheathePressed;

	/** LT + face. Slot indices line up with EARPGElementSlot. */
	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInputSlot OnMagicSelect;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnMagicDiscard;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnMagicPagePrev;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnMagicPageNext;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnQuickSlotPrev;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnQuickSlotNext;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnQuickSlotUse;

	/** RT's rising edge, before any face button has been chosen. */
	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnDischargeModifierPressed;

	/**
	 * RT + face went DOWN. The wind-up starts here.
	 *
	 * Separate from OnDischargeActivated because the charge is not a value this
	 * component computes and hands over at the end -- it is a process something
	 * else runs while the button is held, spending mana as it goes. Reporting
	 * only the release would compress that whole hold into one frame.
	 */
	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInputSlot OnDischargeChargeStarted;

	/**
	 * RT + face came back UP.
	 *
	 * The Charge parameter is what the INPUT layer measured, press to release,
	 * against MaxChargeTime -- a number for a UI meter, and what a consumer that
	 * runs no charge of its own would use. It is not necessarily what the spell
	 * costs or delivers: whatever ran the wind-up is the authority on that.
	 */
	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInputCharge OnDischargeActivated;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Input")
	FARPGOnModalInput OnDischargeCancelled;

	// --- Config ----------------------------------------------------------------

	/** Seconds of hold that reach a full-strength discharge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Input", meta = (ClampMin = "0.01"))
	float MaxChargeTime = 1.5f;

	UPROPERTY(EditAnywhere, Category = "ARPG|Input|Actions")
	TObjectPtr<UInputAction> FaceNorthAction;

	UPROPERTY(EditAnywhere, Category = "ARPG|Input|Actions")
	TObjectPtr<UInputAction> FaceWestAction;

	UPROPERTY(EditAnywhere, Category = "ARPG|Input|Actions")
	TObjectPtr<UInputAction> FaceSouthAction;

	UPROPERTY(EditAnywhere, Category = "ARPG|Input|Actions")
	TObjectPtr<UInputAction> FaceEastAction;

	/** LT. */
	UPROPERTY(EditAnywhere, Category = "ARPG|Input|Actions")
	TObjectPtr<UInputAction> MagicModifierAction;

	/** RT. */
	UPROPERTY(EditAnywhere, Category = "ARPG|Input|Actions")
	TObjectPtr<UInputAction> DischargeModifierAction;

	/** RB. */
	UPROPERTY(EditAnywhere, Category = "ARPG|Input|Actions")
	TObjectPtr<UInputAction> SpecialAttackAction;

	/** LB. */
	UPROPERTY(EditAnywhere, Category = "ARPG|Input|Actions")
	TObjectPtr<UInputAction> ParryAction;

	UPROPERTY(EditAnywhere, Category = "ARPG|Input|Actions")
	TObjectPtr<UInputAction> DPadUpAction;

	UPROPERTY(EditAnywhere, Category = "ARPG|Input|Actions")
	TObjectPtr<UInputAction> DPadDownAction;

	UPROPERTY(EditAnywhere, Category = "ARPG|Input|Actions")
	TObjectPtr<UInputAction> DPadLeftAction;

	UPROPERTY(EditAnywhere, Category = "ARPG|Input|Actions")
	TObjectPtr<UInputAction> DPadRightAction;

protected:
	/**
	 * Re-reads both trigger actions from the player's live input state.
	 *
	 * Enhanced Input's dispatch order makes this belt-and-braces rather than
	 * load-bearing, but the failure it guards against is severe and silent: a
	 * trigger and a face button pressed on the same frame, with the face press
	 * seen first, routes a discharge as a plain attack. The Godot original hit
	 * exactly that and fixed it the same way -- by asking for the live state at
	 * the moment of the press rather than trusting a cached flag.
	 *
	 * A no-op when there is no player input to read, which is what lets tests
	 * and replays drive SetMagicModifier() directly.
	 */
	void PollModifiers();

private:
	void HandleFacePressed(EARPGInputFace Slot);
	void HandleFaceReleased(EARPGInputFace Slot);

	/** Fires the discharge for the slot that is charging and clears the charge. */
	void CompleteCharge();

	bool bMagicModifierHeld = false;
	bool bDischargeModifierHeld = false;
	bool bParryHeld = false;

	EARPGInputFace ChargingSlot = EARPGInputFace::None;
	float ChargeElapsed = 0.f;

	// --- Enhanced Input trampolines -------------------------------------------
	//
	// Enhanced Input binds to a UFUNCTION by name, so each action needs a real
	// member to bind to; the parameterised state machine above cannot be bound
	// directly.

	UFUNCTION() void InputMagicModifierPressed();
	UFUNCTION() void InputMagicModifierReleased();
	UFUNCTION() void InputDischargeModifierPressed();
	UFUNCTION() void InputDischargeModifierReleased();

	UFUNCTION() void InputFaceNorthPressed();
	UFUNCTION() void InputFaceNorthReleased();
	UFUNCTION() void InputFaceWestPressed();
	UFUNCTION() void InputFaceWestReleased();
	UFUNCTION() void InputFaceSouthPressed();
	UFUNCTION() void InputFaceSouthReleased();
	UFUNCTION() void InputFaceEastPressed();
	UFUNCTION() void InputFaceEastReleased();

	UFUNCTION() void InputSpecialPressed();
	UFUNCTION() void InputSpecialReleased();
	UFUNCTION() void InputParryPressed();
	UFUNCTION() void InputParryReleased();

	UFUNCTION() void InputDPadUp();
	UFUNCTION() void InputDPadDown();
	UFUNCTION() void InputDPadLeft();
	UFUNCTION() void InputDPadRight();
};
