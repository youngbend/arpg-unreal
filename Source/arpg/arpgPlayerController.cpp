// Copyright Epic Games, Inc. All Rights Reserved.


#include "arpgPlayerController.h"
#include "ARPGHudWidget.h"
#include "InputMappingContext.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputMappingContext.h"
#include "Blueprint/UserWidget.h"
#include "arpg.h"
#include "Widgets/Input/SVirtualJoystick.h"

void AarpgPlayerController::BeginPlay()
{
	Super::BeginPlay();

	// The HUD is per-viewport, so only the local player builds one -- a
	// listen server's second client has its own controller and its own.
	if (IsLocalPlayerController())
	{
		// The authored Blueprint if there is one, the C++ layout otherwise. NOT a
		// warning when the Blueprint is missing: having none is the normal state,
		// and the fallback is a working HUD rather than a degraded one.
		UClass* WidgetClass = HudWidgetClass.LoadSynchronous();
		if (!WidgetClass)
		{
			WidgetClass = UARPGHudWidget::StaticClass();
		}

		HudWidget = CreateWidget<UUserWidget>(this, WidgetClass);
		if (HudWidget)
		{
			HudWidget->AddToPlayerScreen(0);
		}
	}

	// only spawn touch controls on local player controllers
	if (IsLocalPlayerController() && ShouldUseTouchControls())
	{
		// spawn the mobile controls widget
		MobileControlsWidget = CreateWidget<UUserWidget>(this, MobileControlsWidgetClass);

		if (MobileControlsWidget)
		{
			// add the controls to the player screen
			MobileControlsWidget->AddToPlayerScreen(0);

		} else {

			UE_LOG(Logarpg, Error, TEXT("Could not spawn mobile controls widget."));

		}

	}
}

AarpgPlayerController::AarpgPlayerController()
{
	// A soft path, resolved when input is set up. Nothing loads here.
	ModalMappingContext = TSoftObjectPtr<UInputMappingContext>(
		FSoftObjectPath(TEXT("/Game/ARPG/Input/IMC_ARPG.IMC_ARPG")));

	// A widget Blueprint at this path takes over if one exists; otherwise the
	// C++ class builds its own plain layout. _C is the generated class of the
	// Blueprint, not the asset itself.
	HudWidgetClass = TSoftClassPtr<UUserWidget>(
		FSoftObjectPath(TEXT("/Game/ARPG/UI/WBP_ARPGHud.WBP_ARPGHud_C")));
}

void AarpgPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// only add IMCs for local player controllers
	if (IsLocalPlayerController())
	{
		// Add Input Mapping Contexts
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
		{
			for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
			{
				Subsystem->AddMappingContext(CurrentContext, 0);
			}

			// only add these IMCs if we're not using mobile touch input
			if (!ShouldUseTouchControls())
			{
				for (UInputMappingContext* CurrentContext : MobileExcludedMappingContexts)
				{
					Subsystem->AddMappingContext(CurrentContext, 0);
				}
			}

			// Higher priority than the template's, so a key the modal scheme
			// claims wins where the two ever overlap.
			if (UInputMappingContext* Modal = ModalMappingContext.LoadSynchronous())
			{
				Subsystem->AddMappingContext(Modal, 1);
			}
			else
			{
				UE_LOG(Logarpg, Warning,
					TEXT("No modal input context at '%s'. Face buttons, triggers and the ")
					TEXT("D-pad will do nothing. Run Tools/generate_input_assets.py."),
					*ModalMappingContext.ToString());
			}
		}
	}
}

bool AarpgPlayerController::ShouldUseTouchControls() const
{
	// are we on a mobile platform? Should we force touch?
	return SVirtualJoystick::ShouldDisplayTouchInterface() || bForceTouchControls;
}
