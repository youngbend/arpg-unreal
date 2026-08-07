// Copyright Epic Games, Inc. All Rights Reserved.

#include "arpgCharacter.h"
#include "Engine/LocalPlayer.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/Controller.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "arpg.h"
#include "ARPGPlayerState.h"
#include "ARPGAbilitySystemComponent.h"
#include "ARPGHitboxComponent.h"
#include "ARPGVitalSet.h"
#include "TimerManager.h"

AarpgCharacter::AarpgCharacter()
{
	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.0f);
		
	// Don't rotate when the controller rotates. Let that just affect the camera.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// Configure character movement
	GetCharacterMovement()->bOrientRotationToMovement = true;
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 500.0f, 0.0f);

	// Note: For faster iteration times these variables, and many more, can be tweaked in the Character Blueprint
	// instead of recompiling to adjust them
	GetCharacterMovement()->JumpZVelocity = 500.f;
	GetCharacterMovement()->AirControl = 0.35f;
	GetCharacterMovement()->MaxWalkSpeed = 500.f;
	GetCharacterMovement()->MinAnalogWalkSpeed = 20.f;
	GetCharacterMovement()->BrakingDecelerationWalking = 2000.f;
	GetCharacterMovement()->BrakingDecelerationFalling = 1500.0f;

	// Create a camera boom (pulls in towards the player if there is a collision)
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 400.0f;
	CameraBoom->bUsePawnControlRotation = true;

	// Create a follow camera
	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	// Note: The skeletal mesh and anim blueprint references on the Mesh component (inherited from Character)
	// are set in the derived blueprint asset named ThirdPersonCharacter (to avoid direct content references in C++)

	// Debug hitbox -- see the header. Parked in front of the character rather
	// than on a hand socket: a socket ties this to whichever skeleton the
	// Blueprint happens to assign, and a fixed forward offset is easier to aim
	// deliberately when the point is to verify damage numbers, not swing arcs.
	DebugHitbox = CreateDefaultSubobject<UARPGHitboxComponent>(TEXT("DebugHitbox"));
	DebugHitbox->SetupAttachment(RootComponent);
	DebugHitbox->SetRelativeLocation(FVector(90.f, 0.f, 0.f));
	DebugHitbox->TraceRadius = 60.f;
	DebugHitbox->BaseDamage = 50.f;
	DebugHitbox->PoiseDamage = 10.f;
	DebugHitbox->DeactivateHitbox();
}

void AarpgCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	// Set up action bindings
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent)) {
		
		// Jumping
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);

		// Moving
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AarpgCharacter::Move);
		EnhancedInputComponent->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &AarpgCharacter::Look);

		// Looking
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AarpgCharacter::Look);
	}
	else
	{
		UE_LOG(Logarpg, Error, TEXT("'%s' Failed to find an Enhanced Input component! This template is built to use the Enhanced Input system. If you intend to use the legacy system, then you will need to update this C++ file."), *GetNameSafe(this));
	}
}

void AarpgCharacter::Move(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D MovementVector = Value.Get<FVector2D>();

	// route the input
	DoMove(MovementVector.X, MovementVector.Y);
}

void AarpgCharacter::Look(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D LookAxisVector = Value.Get<FVector2D>();

	// route the input
	DoLook(LookAxisVector.X, LookAxisVector.Y);
}

void AarpgCharacter::DoMove(float Right, float Forward)
{
	if (GetController() != nullptr)
	{
		// find out which way is forward
		const FRotator Rotation = GetController()->GetControlRotation();
		const FRotator YawRotation(0, Rotation.Yaw, 0);

		// get forward vector
		const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);

		// get right vector 
		const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		// add movement 
		AddMovementInput(ForwardDirection, Forward);
		AddMovementInput(RightDirection, Right);
	}
}

void AarpgCharacter::DoLook(float Yaw, float Pitch)
{
	if (GetController() != nullptr)
	{
		// add yaw and pitch input to controller
		AddControllerYawInput(Yaw);
		AddControllerPitchInput(Pitch);
	}
}

void AarpgCharacter::DoJumpStart()
{
	// signal the character to jump
	Jump();
}

void AarpgCharacter::DoJumpEnd()
{
	// signal the character to stop jumping
	StopJumping();
}

UAbilitySystemComponent* AarpgCharacter::GetAbilitySystemComponent() const
{
	return CachedAbilitySystemComponent;
}

void AarpgCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	// Server path: the PlayerState exists by the time possession happens.
	InitAbilityActorInfo();
}

void AarpgCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();

	// Client path: this is the first moment the PlayerState (and so the ASC)
	// is actually available locally.
	InitAbilityActorInfo();
}

void AarpgCharacter::InitAbilityActorInfo()
{
	AARPGPlayerState* ARPGPlayerState = GetPlayerState<AARPGPlayerState>();
	if (!ARPGPlayerState)
	{
		return;
	}

	CachedAbilitySystemComponent = ARPGPlayerState->GetARPGAbilitySystemComponent();
	if (!CachedAbilitySystemComponent)
	{
		return;
	}

	// Owner is the PlayerState (which holds the component); Avatar is this pawn
	// (what abilities animate, trace from, and attach effects to). Getting these
	// the wrong way round is what makes montages fail to play on a possessed
	// pawn while attributes still appear to work.
	CachedAbilitySystemComponent->InitAbilityActorInfo(ARPGPlayerState, this);
}

// --- Debug harness ----------------------------------------------------------

void AarpgCharacter::ARPGSwing()
{
	// Hit detection is server-authoritative (see UARPGHitboxComponent), so a
	// client typing this must ask the server to swing rather than arming its own
	// hitbox and watching nothing happen.
	if (HasAuthority())
	{
		ServerDebugSwing_Implementation();
	}
	else
	{
		ServerDebugSwing();
	}
}

void AarpgCharacter::ServerDebugSwing_Implementation()
{
	if (!DebugHitbox)
	{
		return;
	}

	DebugHitbox->ActivateHitbox();

	GetWorldTimerManager().SetTimer(DebugSwingTimer, this,
		&AarpgCharacter::EndDebugSwing, DebugSwingDuration, /*bLoop=*/false);

	UE_LOG(Logarpg, Log, TEXT("[SERVER] %s debug swing armed for %.2fs (%.0f damage)"),
		*GetName(), DebugSwingDuration, DebugHitbox->BaseDamage);
}

void AarpgCharacter::EndDebugSwing()
{
	if (DebugHitbox)
	{
		DebugHitbox->DeactivateHitbox();
	}
}

void AarpgCharacter::ARPGStats()
{
	const UAbilitySystemComponent* ASC = GetAbilitySystemComponent();
	if (!ASC)
	{
		UE_LOG(Logarpg, Warning, TEXT("%s has no ability system component."), *GetName());
		return;
	}

	// Logged from whichever machine you type it on, deliberately: running this
	// on both ends of a listen-server session is the cheapest proof that
	// attributes actually replicated rather than only changing on the server.
	UE_LOG(Logarpg, Log,
		TEXT("[%s] %s  HP %.1f/%.1f  Stamina %.1f/%.1f  Mana %.1f/%.1f  Poise %.1f/%.1f"),
		HasAuthority() ? TEXT("SERVER") : TEXT("CLIENT"), *GetName(),
		ASC->GetNumericAttribute(UARPGVitalSet::GetHealthAttribute()),
		ASC->GetNumericAttribute(UARPGVitalSet::GetMaxHealthAttribute()),
		ASC->GetNumericAttribute(UARPGVitalSet::GetStaminaAttribute()),
		ASC->GetNumericAttribute(UARPGVitalSet::GetMaxStaminaAttribute()),
		ASC->GetNumericAttribute(UARPGVitalSet::GetManaAttribute()),
		ASC->GetNumericAttribute(UARPGVitalSet::GetMaxManaAttribute()),
		ASC->GetNumericAttribute(UARPGVitalSet::GetPoiseAttribute()),
		ASC->GetNumericAttribute(UARPGVitalSet::GetMaxPoiseAttribute()));
}

void AarpgCharacter::ARPGDebugDraw(bool bEnabled)
{
	if (DebugHitbox)
	{
		DebugHitbox->bDrawDebugTrace = bEnabled;
		UE_LOG(Logarpg, Log, TEXT("Debug hitbox trace drawing %s"),
			bEnabled ? TEXT("ON") : TEXT("OFF"));
	}
}
