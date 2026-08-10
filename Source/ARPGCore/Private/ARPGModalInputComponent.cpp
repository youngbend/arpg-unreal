// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGModalInputComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedPlayerInput.h"
#include "InputAction.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

UARPGModalInputComponent::UARPGModalInputComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	// Off until something is actually charging. The component is otherwise
	// purely event-driven, and a tick that only ever adds zero to a timer is
	// still a tick the whole scheme pays for every frame.
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UARPGModalInputComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (ChargingSlot == EARPGInputFace::None)
	{
		return;
	}

	// Clamped as it accumulates rather than at read time, so a button held for a
	// minute does not carry a charge value that only LOOKS bounded once
	// normalised -- anything reading ChargeElapsed directly sees the same cap.
	ChargeElapsed = FMath::Min(ChargeElapsed + DeltaTime, MaxChargeTime);
}

float UARPGModalInputComponent::GetChargeFraction() const
{
	return MaxChargeTime > 0.f ? FMath::Clamp(ChargeElapsed / MaxChargeTime, 0.f, 1.f) : 0.f;
}

// ---------------------------------------------------------------------------
// Enhanced Input
// ---------------------------------------------------------------------------

void UARPGModalInputComponent::BindActions(UEnhancedInputComponent* Input)
{
	if (!Input)
	{
		return;
	}

	// Modifiers first -- see the header. Enhanced Input dispatches a frame's
	// bindings in the order they were added, so this is what makes a trigger
	// pressed on the same frame as a face button resolve as a modal press.
	if (MagicModifierAction)
	{
		Input->BindAction(MagicModifierAction, ETriggerEvent::Started, this,
			&UARPGModalInputComponent::InputMagicModifierPressed);
		Input->BindAction(MagicModifierAction, ETriggerEvent::Completed, this,
			&UARPGModalInputComponent::InputMagicModifierReleased);
		Input->BindAction(MagicModifierAction, ETriggerEvent::Canceled, this,
			&UARPGModalInputComponent::InputMagicModifierReleased);
	}

	if (DischargeModifierAction)
	{
		Input->BindAction(DischargeModifierAction, ETriggerEvent::Started, this,
			&UARPGModalInputComponent::InputDischargeModifierPressed);
		Input->BindAction(DischargeModifierAction, ETriggerEvent::Completed, this,
			&UARPGModalInputComponent::InputDischargeModifierReleased);
		Input->BindAction(DischargeModifierAction, ETriggerEvent::Canceled, this,
			&UARPGModalInputComponent::InputDischargeModifierReleased);
	}

	const auto BindPressRelease = [Input, this](UInputAction* Action,
		void (UARPGModalInputComponent::*Pressed)(),
		void (UARPGModalInputComponent::*Released)())
	{
		if (!Action)
		{
			return;
		}
		Input->BindAction(Action, ETriggerEvent::Started, this, Pressed);
		if (Released)
		{
			Input->BindAction(Action, ETriggerEvent::Completed, this, Released);
			Input->BindAction(Action, ETriggerEvent::Canceled, this, Released);
		}
	};

	BindPressRelease(FaceNorthAction, &UARPGModalInputComponent::InputFaceNorthPressed,
		&UARPGModalInputComponent::InputFaceNorthReleased);
	BindPressRelease(FaceWestAction, &UARPGModalInputComponent::InputFaceWestPressed,
		&UARPGModalInputComponent::InputFaceWestReleased);
	BindPressRelease(FaceSouthAction, &UARPGModalInputComponent::InputFaceSouthPressed,
		&UARPGModalInputComponent::InputFaceSouthReleased);
	BindPressRelease(FaceEastAction, &UARPGModalInputComponent::InputFaceEastPressed,
		&UARPGModalInputComponent::InputFaceEastReleased);

	BindPressRelease(SpecialAttackAction, &UARPGModalInputComponent::InputSpecialPressed,
		&UARPGModalInputComponent::InputSpecialReleased);
	BindPressRelease(ParryAction, &UARPGModalInputComponent::InputParryPressed,
		&UARPGModalInputComponent::InputParryReleased);

	// The hat is a VALUE, so it binds to Triggered with the value signature
	// rather than to Started -- there is no press edge to catch, only a reading
	// that changes. Bound first so a device that has one is decoded before the
	// four discrete actions get a chance to do nothing.
	if (DPadHatAction)
	{
		Input->BindAction(DPadHatAction, ETriggerEvent::Triggered, this,
			&UARPGModalInputComponent::InputDPadHat);
	}

	BindPressRelease(DPadUpAction, &UARPGModalInputComponent::InputDPadUp, nullptr);
	BindPressRelease(DPadDownAction, &UARPGModalInputComponent::InputDPadDown, nullptr);
	BindPressRelease(DPadLeftAction, &UARPGModalInputComponent::InputDPadLeft, nullptr);
	BindPressRelease(DPadRightAction, &UARPGModalInputComponent::InputDPadRight, nullptr);
}

void UARPGModalInputComponent::PollModifiers()
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const AController* Controller = Pawn ? Pawn->GetController() : Cast<AController>(GetOwner());
	const APlayerController* PC = Cast<APlayerController>(Controller);
	const UEnhancedPlayerInput* PlayerInput = PC ? Cast<UEnhancedPlayerInput>(PC->PlayerInput) : nullptr;

	if (!PlayerInput)
	{
		return;
	}

	if (MagicModifierAction)
	{
		bMagicModifierHeld = PlayerInput->GetActionValue(MagicModifierAction).Get<bool>();
	}

	// Assigned directly rather than through SetDischargeModifier(), which would
	// re-broadcast OnDischargeModifierPressed on a rising edge that the binding
	// is about to report anyway.
	if (DischargeModifierAction)
	{
		bDischargeModifierHeld = PlayerInput->GetActionValue(DischargeModifierAction).Get<bool>();
	}
}

// ---------------------------------------------------------------------------
// Modal state machine
// ---------------------------------------------------------------------------

void UARPGModalInputComponent::SetMagicModifier(bool bHeld)
{
	bMagicModifierHeld = bHeld;
}

void UARPGModalInputComponent::SetDischargeModifier(bool bHeld)
{
	if (bHeld == bDischargeModifierHeld)
	{
		return;
	}

	bDischargeModifierHeld = bHeld;

	// Only the rising edge is announced. The receiving side uses it to auto-ready
	// an element when the player pulls RT with nothing selected, so that pulling
	// the trigger alone is enough to cast -- the falling edge means nothing,
	// because a charge already under way is not RT's to end.
	if (bHeld)
	{
		OnDischargeModifierPressed.Broadcast();
	}
}

void UARPGModalInputComponent::PressFace(EARPGInputFace Slot)
{
	if (Slot == EARPGInputFace::None)
	{
		return;
	}

	PollModifiers();
	HandleFacePressed(Slot);
}

void UARPGModalInputComponent::ReleaseFace(EARPGInputFace Slot)
{
	if (Slot == EARPGInputFace::None)
	{
		return;
	}

	HandleFaceReleased(Slot);
}

void UARPGModalInputComponent::HandleFacePressed(EARPGInputFace Slot)
{
	// RT wins when both triggers are down. Starting a charge is the more
	// committed action of the two, and readying an element is a step on the way
	// to it -- so the ambiguous grip resolves towards the thing the player is
	// further into doing.
	if (bDischargeModifierHeld)
	{
		// One charge at a time; a second face button pressed mid-charge is
		// ignored rather than stealing the slot, so a fumbled grip cannot
		// silently swap which element is about to come out.
		if (ChargingSlot == EARPGInputFace::None)
		{
			ChargingSlot = Slot;
			ChargeElapsed = 0.f;
			SetComponentTickEnabled(true);
			OnDischargeChargeStarted.Broadcast(Slot);
		}
		return;
	}

	if (bMagicModifierHeld)
	{
		// Instantly, on PRESS -- no release needed. Readying an element is a
		// modal state the player then acts from, so it has to be true while the
		// thumb is still on the button.
		OnMagicSelect.Broadcast(Slot);
		return;
	}

	switch (Slot)
	{
	case EARPGInputFace::North: OnHeavyAttack.Broadcast(); break;
	case EARPGInputFace::West:  OnLightAttack.Broadcast(); break;
	case EARPGInputFace::South: OnJump.Broadcast();        break;
	case EARPGInputFace::East:  OnDodge.Broadcast();       break;
	default: break;
	}
}

void UARPGModalInputComponent::HandleFaceReleased(EARPGInputFace Slot)
{
	// Checked BEFORE the modifiers, and that ordering is the rule: a charge
	// completes on its own button coming up whatever the triggers are doing by
	// then. RT can have been released, or LT picked up, and the discharge the
	// player wound up still fires.
	if (ChargingSlot == Slot)
	{
		CompleteCharge();
		return;
	}

	// A face release while either modifier is held is swallowed. Without this,
	// letting go of X after readying an element also fires a light-attack
	// release into the combo component, which ends a charge the player is still
	// holding elsewhere in the kit.
	if (bDischargeModifierHeld || bMagicModifierHeld)
	{
		return;
	}

	switch (Slot)
	{
	case EARPGInputFace::North: OnHeavyAttackReleased.Broadcast(); break;
	case EARPGInputFace::West:  OnLightAttackReleased.Broadcast(); break;
	// South and East -- jump and dodge -- have no release semantic. Both are
	// impulses; neither charges, channels, or holds a stance.
	default: break;
	}
}

void UARPGModalInputComponent::CompleteCharge()
{
	const float Charge = GetChargeFraction();
	const EARPGInputFace Slot = ChargingSlot;

	ChargingSlot = EARPGInputFace::None;
	ChargeElapsed = 0.f;
	SetComponentTickEnabled(false);

	OnDischargeActivated.Broadcast(Slot, Charge);
}

void UARPGModalInputComponent::CancelCharge()
{
	if (ChargingSlot == EARPGInputFace::None)
	{
		return;
	}

	ChargingSlot = EARPGInputFace::None;
	ChargeElapsed = 0.f;
	SetComponentTickEnabled(false);

	OnDischargeCancelled.Broadcast();
}

void UARPGModalInputComponent::PressSpecial()
{
	PollModifiers();

	// Swallowed under either modifier, matching the face buttons: with a trigger
	// down the whole right hand is in modal territory, and RB firing a special
	// attack out of it would be the one control that ignores the mode.
	if (bDischargeModifierHeld || bMagicModifierHeld)
	{
		return;
	}

	OnSpecialAttack.Broadcast();
}

void UARPGModalInputComponent::ReleaseSpecial()
{
	if (bDischargeModifierHeld || bMagicModifierHeld)
	{
		return;
	}

	OnSpecialAttackReleased.Broadcast();
}

void UARPGModalInputComponent::PressParry()
{
	if (bParryHeld)
	{
		return;
	}

	bParryHeld = true;
	OnParryPressed.Broadcast();
}

void UARPGModalInputComponent::ReleaseParry()
{
	if (!bParryHeld)
	{
		return;
	}

	bParryHeld = false;
	OnParryReleased.Broadcast();
}

void UARPGModalInputComponent::PressDPad(EARPGInputDPad Direction)
{
	PollModifiers();

	switch (Direction)
	{
	case EARPGInputDPad::Down:
		// LT + Down discards what is readied; Down alone sheathes. Both are
		// "put that away", which is why they share a button.
		if (bMagicModifierHeld)
		{
			OnMagicDiscard.Broadcast();
		}
		else
		{
			OnSheathePressed.Broadcast();
		}
		break;

	case EARPGInputDPad::Up:
		// NOTHING is bound to LT + Up, and the press is still consumed -- see
		// the header. Falling through here would drink a potion because the
		// player was a frame late letting go of LT.
		if (!bMagicModifierHeld)
		{
			OnQuickSlotUse.Broadcast();
		}
		break;

	case EARPGInputDPad::Left:
		if (bMagicModifierHeld)
		{
			OnMagicPagePrev.Broadcast();
		}
		else
		{
			OnQuickSlotPrev.Broadcast();
		}
		break;

	case EARPGInputDPad::Right:
		if (bMagicModifierHeld)
		{
			OnMagicPageNext.Broadcast();
		}
		else
		{
			OnQuickSlotNext.Broadcast();
		}
		break;
	}
}

// ---------------------------------------------------------------------------
// Enhanced Input trampolines
// ---------------------------------------------------------------------------

void UARPGModalInputComponent::InputMagicModifierPressed() { SetMagicModifier(true); }
void UARPGModalInputComponent::InputMagicModifierReleased() { SetMagicModifier(false); }
void UARPGModalInputComponent::InputDischargeModifierPressed() { SetDischargeModifier(true); }
void UARPGModalInputComponent::InputDischargeModifierReleased() { SetDischargeModifier(false); }

void UARPGModalInputComponent::InputFaceNorthPressed() { PressFace(EARPGInputFace::North); }
void UARPGModalInputComponent::InputFaceNorthReleased() { ReleaseFace(EARPGInputFace::North); }
void UARPGModalInputComponent::InputFaceWestPressed() { PressFace(EARPGInputFace::West); }
void UARPGModalInputComponent::InputFaceWestReleased() { ReleaseFace(EARPGInputFace::West); }
void UARPGModalInputComponent::InputFaceSouthPressed() { PressFace(EARPGInputFace::South); }
void UARPGModalInputComponent::InputFaceSouthReleased() { ReleaseFace(EARPGInputFace::South); }
void UARPGModalInputComponent::InputFaceEastPressed() { PressFace(EARPGInputFace::East); }
void UARPGModalInputComponent::InputFaceEastReleased() { ReleaseFace(EARPGInputFace::East); }

void UARPGModalInputComponent::InputSpecialPressed() { PressSpecial(); }
void UARPGModalInputComponent::InputSpecialReleased() { ReleaseSpecial(); }
void UARPGModalInputComponent::InputParryPressed() { PressParry(); }
void UARPGModalInputComponent::InputParryReleased() { ReleaseParry(); }

void UARPGModalInputComponent::InputDPadHat(const FInputActionValue& Value)
{
	// Back off the offset the device config added, then recover the raw 0-7
	// compass index. Anything beyond that is the released reading.
	const float Raw = Value.Get<float>() - HatOffset;
	const int32 Direction = FMath::RoundToInt(Raw * 7.f);

	// ONLY ON A CHANGE. A hat reports its position continuously rather than on a
	// transition, so acting every frame would page the loadout as fast as the
	// game ticks for as long as the player held left.
	if (Direction == LastHatDirection)
	{
		return;
	}
	LastHatDirection = Direction;

	// 0-7 clockwise from north. The diagonals resolve to their vertical
	// component, because a diagonal on a D-pad is a mis-press of one of the two
	// cardinals -- and of the pair, up and down are the ones with consequences
	// worth protecting (drinking a potion, discarding what is readied).
	switch (Direction)
	{
	case 0: case 1: case 7: PressDPad(EARPGInputDPad::Up);    break;
	case 3: case 4: case 5: PressDPad(EARPGInputDPad::Down);  break;
	case 2:                 PressDPad(EARPGInputDPad::Right); break;
	case 6:                 PressDPad(EARPGInputDPad::Left);  break;
	default: break; // released, or a reading outside the hat's range
	}
}

void UARPGModalInputComponent::InputDPadUp() { PressDPad(EARPGInputDPad::Up); }
void UARPGModalInputComponent::InputDPadDown() { PressDPad(EARPGInputDPad::Down); }
void UARPGModalInputComponent::InputDPadLeft() { PressDPad(EARPGInputDPad::Left); }
void UARPGModalInputComponent::InputDPadRight() { PressDPad(EARPGInputDPad::Right); }
