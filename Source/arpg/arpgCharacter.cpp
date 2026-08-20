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
#include "InputAction.h"
#include "InputActionValue.h"
#include "arpg.h"
#include "ARPGPlayerState.h"
#include "ARPGAbilitySystemComponent.h"
#include "ARPGHitboxComponent.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGArmorComponent.h"
#include "ARPGCloakComponent.h"
#include "ARPGCombatProgressionTrackers.h"
#include "ARPGComboComponent.h"
#include "ARPGHitStopComponent.h"
#include "ARPGHurtboxComponent.h"
#include "ARPGMagicProgressionTracker.h"
#include "ARPGPoiseComponent.h"
#include "ARPGProgressionComponent.h"
#include "ARPGStatusResistanceComponent.h"
#include "ARPGInventoryComponent.h"
#include "ARPGHandVisualComponent.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicComponent.h"
#include "ARPGMagicLoadout.h"
#include "ARPGParryComponent.h"
#include "ARPGQuickSlotComponent.h"
#include "ARPGWeaponComponent.h"
#include "ARPGWeaponDefinition.h"
#include "ARPGLocomotionComponent.h"
#include "ARPGNoiseComponent.h"
#include "ARPGVitalRegenComponent.h"
#include "ARPGModalInputComponent.h"
#include "ARPGPlayerActionComponent.h"
#include "ARPGWeaponAttackTree.h"
#include "ARPGAttackDefinition.h"
#include "ARPGGameplayAbility_Discharge.h"
#include "ARPGGameplayAbility_Dodge.h"
#include "ARPGGameplayAbility_Imbue.h"
#include "ARPGGameplayAbility_MeleeAttack.h"
#include "Abilities/GameplayAbility.h"
#include "ARPGVitalSet.h"
#include "TimerManager.h"

AarpgCharacter::AarpgCharacter()
{
	// Ticks only to service ARPGPadInput, which is idle unless asked for.
	PrimaryActorTick.bCanEverTick = true;

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

	// Soft path only -- nothing loads here. See the header for why the debug
	// harness names content at all.
	DebugDamageType = TSoftObjectPtr<UARPGDamageTypeAsset>(
		FSoftObjectPath(TEXT("/Game/ARPG/DamageTypes/DA_Damage_Physical.DA_Damage_Physical")));

	ComboComponent = CreateDefaultSubobject<UARPGComboComponent>(TEXT("ComboComponent"));

	// The modal scheme and the two components that consume it. Created in C++
	// rather than added per-Blueprint because the three only work together: an
	// input component with nothing listening is silent, and an action component
	// with no input to bind to never fires.
	ModalInput = CreateDefaultSubobject<UARPGModalInputComponent>(TEXT("ModalInput"));
	Locomotion = CreateDefaultSubobject<UARPGLocomotionComponent>(TEXT("Locomotion"));
	VitalRegen = CreateDefaultSubobject<UARPGVitalRegenComponent>(TEXT("VitalRegen"));
	PlayerActions = CreateDefaultSubobject<UARPGPlayerActionComponent>(TEXT("PlayerActions"));

	WeaponComponent = CreateDefaultSubobject<UARPGWeaponComponent>(TEXT("WeaponComponent"));
	ParryComponent = CreateDefaultSubobject<UARPGParryComponent>(TEXT("ParryComponent"));
	MagicComponent = CreateDefaultSubobject<UARPGMagicComponent>(TEXT("MagicComponent"));
	HandVisual = CreateDefaultSubobject<UARPGHandVisualComponent>(TEXT("HandVisual"));

	// Inventory before quick slots reads like ordering that does not matter, and
	// it does not -- the quick-slot component resolves its inventory lazily by
	// class, not by construction order. Kept adjacent because they are one
	// feature: a bar with nothing behind it has nothing to hand out.
	InventoryComponent = CreateDefaultSubobject<UARPGInventoryComponent>(TEXT("InventoryComponent"));
	QuickSlotComponent = CreateDefaultSubobject<UARPGQuickSlotComponent>(TEXT("QuickSlotComponent"));

	// What NPCs hear. Without it the player is silent by construction and the
	// hearing channel does nothing on the one actor it matters most for -- see
	// the header.
	NoiseComponent = CreateDefaultSubobject<UARPGNoiseComponent>(TEXT("NoiseComponent"));

	// --- What the player is on the receiving end of --------------------------
	//
	// See the header. Each of these was implemented, tested and attached to
	// nothing; the tests built their own actors and added them by hand, so the
	// suite stayed green while the real pawn went without.

	// FIRST, because without it the player is not a damageable thing at all --
	// UARPGHitboxComponent skips any actor whose hurtbox does not resolve.
	Hurtbox = CreateDefaultSubobject<UARPGHurtboxComponent>(TEXT("Hurtbox"));

	PoiseComponent   = CreateDefaultSubobject<UARPGPoiseComponent>(TEXT("PoiseComponent"));
	ArmorComponent   = CreateDefaultSubobject<UARPGArmorComponent>(TEXT("ArmorComponent"));
	HitStopComponent = CreateDefaultSubobject<UARPGHitStopComponent>(TEXT("HitStopComponent"));
	StatusResistance = CreateDefaultSubobject<UARPGStatusResistanceComponent>(TEXT("StatusResistance"));
	CloakComponent   = CreateDefaultSubobject<UARPGCloakComponent>(TEXT("CloakComponent"));

	// Progression: the visible level and the three hidden trackers. Each tracker
	// binds its own XP source in BeginPlay -- weapon to hits landed, armour to
	// hits received, magic to discharges cast -- so attaching them is the whole
	// of the wiring.
	Progression       = CreateDefaultSubobject<UARPGProgressionComponent>(TEXT("Progression"));
	WeaponProgression = CreateDefaultSubobject<UARPGWeaponProgressionTracker>(TEXT("WeaponProgression"));
	ArmorProgression  = CreateDefaultSubobject<UARPGArmorProgressionTracker>(TEXT("ArmorProgression"));
	MagicProgression  = CreateDefaultSubobject<UARPGMagicProgressionTracker>(TEXT("MagicProgression"));

	// The movement component's own default is what the character runs at, so the
	// tiers are anchored to it rather than to a second set of numbers that could
	// silently disagree with the Blueprint's.
	Locomotion->RunSpeed = GetCharacterMovement()->MaxWalkSpeed;
	Locomotion->WalkSpeed = Locomotion->RunSpeed * 0.4f;
	Locomotion->SprintSpeed = Locomotion->RunSpeed * 1.5f;

	// Anchored to the locomotion tiers for the same reason those are anchored to
	// the movement component: the noise component's own default threshold is 500,
	// which happens to equal RunSpeed here -- so merely running would have counted
	// as sprinting and the loudest tier would have been the normal one. Just under
	// the sprint tier means only an actual sprint is sprint-loud.
	NoiseComponent->SprintSpeedThreshold = Locomotion->SprintSpeed * 0.95f;
	NoiseComponent->IdleSpeedThreshold = Locomotion->WalkSpeed * 0.5f;

	// The melee ability is the one thing without which the whole attack path is
	// silently inert, so it is defaulted here rather than left to per-Blueprint
	// setup.
	DefaultAbilities.Add(UARPGGameplayAbility_MeleeAttack::StaticClass());

	// A gameplay-event or tag activation only fires if the ability has been
	// GRANTED first -- asking an ability system to activate something it was
	// never given does nothing at all, with no error to say so. Every action the
	// modal scheme can reach has to be in this list or the button is dead.
	//
	// The four discharges are separate classes rather than one parameterised
	// ability because the face button chooses between them by tag; see
	// UARPGPlayerActionComponent::FaceToDischargeAbilityTag.
	DefaultAbilities.Add(UARPGGameplayAbility_DischargeProject::StaticClass());
	DefaultAbilities.Add(UARPGGameplayAbility_DischargeBurst::StaticClass());
	DefaultAbilities.Add(UARPGGameplayAbility_DischargeEmanate::StaticClass());
	DefaultAbilities.Add(UARPGGameplayAbility_DischargeCloak::StaticClass());

	DefaultAbilities.Add(UARPGGameplayAbility_Dodge::StaticClass());
	DefaultAbilities.Add(UARPGGameplayAbility_Imbue::StaticClass());

	// A convenience default only. SET THIS ON THE BLUEPRINT -- a hard-coded
	// content path in C++ silently stops working the moment the asset is moved
	// or renamed, which is exactly what happened the first time.
	//
	// THE WEAPON, not the attack tree. This used to point the character's
	// FallbackAttackTree -- the bare-handed moveset -- straight at the sword's
	// tree, so every sword combo was available with nothing equipped and
	// unequipping took nothing away. The sword's moveset now arrives the way any
	// weapon's does: by equipping the weapon that owns it.
	DefaultWeapon = TSoftObjectPtr<UARPGWeaponDefinition>(
		FSoftObjectPath(TEXT("/Game/ARPG/Weapons/sword/DA_Weapon_Sword.DA_Weapon_Sword")));
}

void AarpgCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	// Set up action bindings
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent)) {
		
		// Jump is NOT bound here. It belongs to the modal scheme's South face
		// button, which is the only thing that knows whether a trigger is held --
		// bind it directly and A jumps even while LT is readying an element,
		// which is exactly the misfire the modality exists to prevent.
		//
		// StopJumping still needs the release, and the modal component does not
		// report one for South (jump is an impulse), so it is taken from the
		// action directly. Harmless when nothing is jumping.
		if (JumpAction)
		{
			EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this,
				&ACharacter::StopJumping);
		}

		// Moving
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AarpgCharacter::Move);
		EnhancedInputComponent->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &AarpgCharacter::Look);

		// Looking
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AarpgCharacter::Look);

		// Sprint toggle
		if (SprintAction)
		{
			EnhancedInputComponent->BindAction(SprintAction, ETriggerEvent::Started, this,
				&AarpgCharacter::ToggleSprint);
		}

		// The whole modal scheme in one call. LAST, so the template's own
		// bindings above keep their dispatch order -- and first among the modal
		// bindings themselves are the two triggers, which is what makes a
		// same-frame trigger-plus-face press resolve as modal. See BindActions.
		if (ModalInput)
		{
			ResolveDefaultModalActions();
			ModalInput->BindActions(EnhancedInputComponent);
		}
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

void AarpgCharacter::BeginPlay()
{
	Super::BeginPlay();
	ResolveDefaultMagicContent();
	ResolveDefaultCombatContent();
}

void AarpgCharacter::ResolveDefaultCombatContent()
{
	// The unarmed moveset. Left null unless a Blueprint authors one, and a null
	// one is not an error: it means a character with empty hands has no attacks,
	// which is the honest answer until fists are authored. The combo component
	// says so on the first press.
	if (ComboComponent && !ComboComponent->FallbackAttackTree && !UnarmedAttackTree.IsNull())
	{
		ComboComponent->FallbackAttackTree = UnarmedAttackTree.LoadSynchronous();
		if (!ComboComponent->FallbackAttackTree)
		{
			UE_LOG(Logarpg, Warning,
				TEXT("Unarmed attack tree '%s' failed to load; bare hands will have no attacks."),
				*UnarmedAttackTree.ToString());
		}
	}

	// Server-side, and only when nothing is equipped already: the weapon
	// component equips its own DefaultWeapon in its BeginPlay, which has already
	// run by the time this does, and an NPC-style definition may have equipped
	// something too. Weapon state replicates, so clients receive it rather than
	// resolving it a second time.
	if (!WeaponComponent || !HasAuthority() || WeaponComponent->GetWeapon() || DefaultWeapon.IsNull())
	{
		return;
	}

	if (UARPGWeaponDefinition* Definition = DefaultWeapon.LoadSynchronous())
	{
		WeaponComponent->EquipWeapon(Definition);
	}
	else
	{
		UE_LOG(Logarpg, Error,
			TEXT("Default weapon '%s' failed to load -- the character spawns unarmed and, with no "
			     "unarmed moveset authored, cannot attack at all. Run "
			     "Tools/generate_sword_weapon.py to build it, or set DefaultWeapon on the "
			     "character Blueprint."),
			*DefaultWeapon.ToString());
	}
}

void AarpgCharacter::ResolveDefaultMagicContent()
{
	if (!MagicComponent)
	{
		return;
	}

	// Soft paths resolved at begin play rather than hard references in the
	// constructor: a moved asset should mean one warning and a character with no
	// spells, not a class that fails to load.
	if (!MagicComponent->Loadout)
	{
		MagicComponent->Loadout = Cast<UARPGMagicLoadout>(FSoftObjectPath(
			TEXT("/Game/ARPG/Magic/DA_MagicLoadout_Starter.DA_MagicLoadout_Starter")).TryLoad());

		if (!MagicComponent->Loadout)
		{
			UE_LOG(Logarpg, Warning,
				TEXT("No starter magic loadout. Element slots will be empty; ")
				TEXT("run Tools/generate_element_assets.py."));
		}
	}

	if (!MagicComponent->CombinationTable)
	{
		MagicComponent->CombinationTable = Cast<UARPGMagicCombinationTable>(FSoftObjectPath(
			TEXT("/Game/ARPG/Magic/DA_MagicCombinations.DA_MagicCombinations")).TryLoad());

		if (!MagicComponent->CombinationTable)
		{
			UE_LOG(Logarpg, Warning,
				TEXT("No magic combination table. Elements will ready but never combine."));
		}
	}
}

void AarpgCharacter::ResolveDefaultModalActions()
{
	if (!ModalInput)
	{
		return;
	}

	// Filled in only where a Blueprint left the slot empty, so overriding one
	// action does not mean re-pointing all thirteen. Resolved HERE rather than in
	// the constructor because these are content paths: a constructor-time load
	// would make the class fail to compile the moment an asset moved, where this
	// merely warns and leaves that one control dead.
	static const TCHAR* const AssetDir = TEXT("/Game/ARPG/Input/");

	// Generic, because the modal component stores its actions as TObjectPtr and
	// the character's own sprint action is a raw pointer -- the assignment and
	// the null test read identically for both.
	auto Resolve = [](auto& Slot, const TCHAR* AssetName)
	{
		if (Slot)
		{
			return; // authored on the Blueprint; leave it alone
		}

		const FString Path = FString::Printf(TEXT("%s%s.%s"), AssetDir, AssetName, AssetName);
		Slot = Cast<UInputAction>(FSoftObjectPath(Path).TryLoad());

		if (!Slot)
		{
			UE_LOG(Logarpg, Warning,
				TEXT("Missing input action '%s'. That control will do nothing; ")
				TEXT("run Tools/generate_input_assets.py."), AssetName);
		}
	};

	Resolve(ModalInput->FaceNorthAction,         TEXT("IA_ARPG_FaceNorth"));
	Resolve(ModalInput->FaceWestAction,          TEXT("IA_ARPG_FaceWest"));
	Resolve(ModalInput->FaceSouthAction,         TEXT("IA_ARPG_FaceSouth"));
	Resolve(ModalInput->FaceEastAction,          TEXT("IA_ARPG_FaceEast"));
	Resolve(ModalInput->MagicModifierAction,     TEXT("IA_ARPG_MagicModifier"));
	Resolve(ModalInput->DischargeModifierAction, TEXT("IA_ARPG_DischargeModifier"));
	Resolve(ModalInput->SpecialAttackAction,     TEXT("IA_ARPG_SpecialAttack"));
	Resolve(ModalInput->ParryAction,             TEXT("IA_ARPG_Parry"));
	Resolve(ModalInput->DPadUpAction,            TEXT("IA_ARPG_DPadUp"));
	Resolve(ModalInput->DPadDownAction,          TEXT("IA_ARPG_DPadDown"));
	Resolve(ModalInput->DPadLeftAction,          TEXT("IA_ARPG_DPadLeft"));
	Resolve(ModalInput->DPadRightAction,         TEXT("IA_ARPG_DPadRight"));


	Resolve(SprintAction, TEXT("IA_ARPG_Sprint"));
}

namespace
{
	/**
	 * The analogue controls, in the order they are worth reading.
	 *
	 * Named individually rather than swept numerically because these are real
	 * keys with meanings -- if LeftX never moves, that sentence is the diagnosis.
	 */
	const TCHAR* const PadAxisKeys[] = {
		TEXT("Gamepad_LeftX"), TEXT("Gamepad_LeftY"),
		TEXT("Gamepad_RightX"), TEXT("Gamepad_RightY"),
		TEXT("Gamepad_LeftTriggerAxis"), TEXT("Gamepad_RightTriggerAxis"),
	};

	const TCHAR* const PadButtonKeys[] = {
		TEXT("Gamepad_FaceButton_Bottom"), TEXT("Gamepad_FaceButton_Right"),
		TEXT("Gamepad_FaceButton_Left"),   TEXT("Gamepad_FaceButton_Top"),
		TEXT("Gamepad_LeftShoulder"),      TEXT("Gamepad_RightShoulder"),
		TEXT("Gamepad_LeftTrigger"),       TEXT("Gamepad_RightTrigger"),
		TEXT("Gamepad_DPad_Up"),           TEXT("Gamepad_DPad_Down"),
		TEXT("Gamepad_DPad_Left"),         TEXT("Gamepad_DPad_Right"),
		TEXT("Gamepad_LeftThumbstick"),    TEXT("Gamepad_RightThumbstick"),
		TEXT("Gamepad_Special_Left"),      TEXT("Gamepad_Special_Right"),
	};

	/** Movement smaller than this is a stick at rest, not the player. */
	constexpr float PadAxisChangeThreshold = 0.08f;
}

void AarpgCharacter::ARPGPadInput(float Seconds)
{
	const APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC || !PC->PlayerInput)
	{
		UE_LOG(Logarpg, Warning, TEXT("ARPGPadInput: no player input to read."));
		return;
	}

	PadWatchLastAxis.Init(0.f, UE_ARRAY_COUNT(PadAxisKeys));

	UE_LOG(Logarpg, Log, TEXT("ARPGPadInput: watching for %.0f seconds."), Seconds);

	// Resting values first. A stick that rests at a steady non-zero value is
	// arriving and merely needs a deadzone; one that reads exactly zero and never
	// changes is not arriving at all.
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(PadAxisKeys); ++Index)
	{
		const float Value = PC->PlayerInput->GetKeyValue(FKey(PadAxisKeys[Index]));
		PadWatchLastAxis[Index] = Value;
		UE_LOG(Logarpg, Log, TEXT("ARPGPadInput:   %s rests at %.3f"),
			PadAxisKeys[Index], Value);
	}

	PadWatchTimer = FMath::Max(Seconds, 0.5f);
}

void AarpgCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (PadWatchTimer <= 0.f)
	{
		return;
	}

	PadWatchTimer -= DeltaSeconds;

	const APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC || !PC->PlayerInput)
	{
		PadWatchTimer = 0.f;
		return;
	}

	for (int32 Index = 0; Index < UE_ARRAY_COUNT(PadAxisKeys); ++Index)
	{
		const float Value = PC->PlayerInput->GetKeyValue(FKey(PadAxisKeys[Index]));

		// Reported on CHANGE rather than on magnitude, so an axis that rests at a
		// non-zero value still shows up the moment it is actually moved.
		if (FMath::Abs(Value - PadWatchLastAxis[Index]) < PadAxisChangeThreshold)
		{
			continue;
		}

		UE_LOG(Logarpg, Log, TEXT("ARPGPadInput: %s -> %.3f"), PadAxisKeys[Index], Value);
		PadWatchLastAxis[Index] = Value;
	}

	for (const TCHAR* const KeyName : PadButtonKeys)
	{
		if (PC->PlayerInput->WasJustPressed(FKey(KeyName)))
		{
			UE_LOG(Logarpg, Log, TEXT("ARPGPadInput: %s pressed"), KeyName);
		}
	}

	if (PadWatchTimer <= 0.f)
	{
		UE_LOG(Logarpg, Log, TEXT("ARPGPadInput: done."));
	}
}

void AarpgCharacter::ToggleSprint()
{
	if (Locomotion)
	{
		Locomotion->ToggleSprint();
	}
}

void AarpgCharacter::DoMove(float Right, float Forward)
{
	if (Locomotion)
	{
		// How hard the stick is pushed, before the camera turns it into a
		// direction. Centring it ends a sprint, and a sprint in progress ignores
		// the magnitude entirely -- see UARPGLocomotionComponent.
		Locomotion->SetMoveMagnitude(FVector2D(Right, Forward).Size());

		if (Locomotion->ShouldIgnoreInputMagnitude())
		{
			const FVector2D Normalised = FVector2D(Right, Forward).GetSafeNormal();
			Right = static_cast<float>(Normalised.X);
			Forward = static_cast<float>(Normalised.Y);
		}
	}

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

	// Granting is server-only and must happen exactly once: InitAbilityActorInfo
	// runs again on respawn and on late PlayerState replication, and re-granting
	// would stack duplicate specs.
	if (HasAuthority() && !bAbilitiesGranted)
	{
		bAbilitiesGranted = true;
		for (const TSubclassOf<UGameplayAbility>& AbilityClass : DefaultAbilities)
		{
			if (AbilityClass)
			{
				CachedAbilitySystemComponent->GiveAbility(
					FGameplayAbilitySpec(AbilityClass, 1, INDEX_NONE, this));
			}
		}
	}

	// Movesets are resolved in ResolveDefaultCombatContent, off BeginPlay. They
	// were resolved here when this function was the only place guaranteed to run
	// on both sides, but the combo component is not replicated -- a client has to
	// fill its own unarmed slot, and BeginPlay is where both sides meet.
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

	// Resolved lazily rather than in the constructor, so a missing or moved
	// asset degrades to the execution's warning instead of a load failure at
	// class-construction time.
	if (!DebugHitbox->DamageType && !DebugDamageType.IsNull())
	{
		DebugHitbox->DamageType = DebugDamageType.LoadSynchronous();
		if (!DebugHitbox->DamageType)
		{
			UE_LOG(Logarpg, Warning,
				TEXT("Debug swing could not load damage type '%s'; the hit will be unmitigated."),
				*DebugDamageType.ToString());
		}
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

void AarpgCharacter::ARPGAttack()
{
	if (HasAuthority()) { ServerComboInput_Implementation(false); }
	else { ServerComboInput(false); }
}

void AarpgCharacter::ARPGAttackHeavy()
{
	if (HasAuthority()) { ServerComboInput_Implementation(true); }
	else { ServerComboInput(true); }
}

void AarpgCharacter::ServerComboInput_Implementation(bool bHeavy)
{
	if (!ComboComponent)
	{
		return;
	}

	ComboComponent->ReceiveInput(bHeavy ? EARPGAttackInput::Heavy : EARPGAttackInput::Light);

	// Released immediately: a console command has no held state, and leaving the
	// input latched would make every continuous_hold attack chain forever.
	ComboComponent->ReceiveInputReleased(bHeavy ? EARPGAttackInput::Heavy : EARPGAttackInput::Light);
}

void AarpgCharacter::ARPGComboState()
{
	const UAbilitySystemComponent* ASC = GetAbilitySystemComponent();
	const int32 AbilityCount = ASC ? ASC->GetActivatableAbilities().Num() : -1;

	FString Node = TEXT("<none>");
	if (ComboComponent && ComboComponent->GetCurrentNode() && ComboComponent->GetCurrentNode()->Attack)
	{
		Node = ComboComponent->GetCurrentNode()->Attack->AttackId.ToString();
	}

	UE_LOG(Logarpg, Log,
		TEXT("[%s] combo node=%s attacking=%d seq=%d tree=%s abilities=%d"),
		HasAuthority() ? TEXT("SERVER") : TEXT("CLIENT"), *Node,
		ComboComponent && ComboComponent->IsAttacking(),
		ComboComponent ? ComboComponent->GetAttackSequenceNumber() : -1,
		ComboComponent && ComboComponent->GetActiveTree()
			? *ComboComponent->GetActiveTree()->GetName() : TEXT("<none>"),
		AbilityCount);
}
