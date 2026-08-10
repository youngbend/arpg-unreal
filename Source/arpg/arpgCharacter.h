// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Logging/LogMacros.h"
#include "AbilitySystemInterface.h"
#include "arpgCharacter.generated.h"

class USpringArmComponent;
class UCameraComponent;
class UInputAction;
class UARPGAbilitySystemComponent;
class UARPGHitboxComponent;
class UARPGDamageTypeAsset;
class UARPGComboComponent;
class UARPGInventoryComponent;
class UARPGLocomotionComponent;
class UARPGHandVisualComponent;
class UARPGMagicComponent;
class UARPGParryComponent;
class UARPGQuickSlotComponent;
class UARPGWeaponComponent;
class UARPGModalInputComponent;
class UARPGPlayerActionComponent;
class UARPGWeaponAttackTree;
class UGameplayAbility;
struct FInputActionValue;

DECLARE_LOG_CATEGORY_EXTERN(LogTemplateCharacter, Log, All);

/**
 *  A simple player-controllable third person character
 *  Implements a controllable orbiting camera
 *
 *  Implements IAbilitySystemInterface so anything holding this actor can find
 *  its ASC without knowing the ASC actually lives on the PlayerState.
 */
UCLASS(abstract)
class AarpgCharacter : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

	/** Camera boom positioning the camera behind the character */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	USpringArmComponent* CameraBoom;

	/** Follow camera */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	UCameraComponent* FollowCamera;
	
protected:

	/** Jump Input Action */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* JumpAction;

	/** Move Input Action */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* MoveAction;

	/** Look Input Action */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* LookAction;

	/** Mouse Look Input Action */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* MouseLookAction;

	/**
	 *  Sprint toggle. A TOGGLE, not a hold -- see UARPGLocomotionComponent.
	 *
	 *  Bound here rather than on the modal input component because sprinting is
	 *  not modal: it means the same thing whichever trigger is down, so routing
	 *  it through a component whose entire job is deciding what a button means
	 *  under a modifier would be putting it in the wrong place.
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* SprintAction;

public:

	/** Constructor */
	AarpgCharacter();	

protected:

	/** Initialize input action bindings */
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

protected:

	/** Called for movement input */
	void Move(const FInputActionValue& Value);

	/** Called for looking input */
	void Look(const FInputActionValue& Value);

public:

	/** Handles move inputs from either controls or UI interfaces */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoMove(float Right, float Forward);

	/** Handles look inputs from either controls or UI interfaces */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoLook(float Yaw, float Pitch);

	/** Handles jump pressed inputs from either controls or UI interfaces */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoJumpStart();

	/** Handles jump pressed inputs from either controls or UI interfaces */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoJumpEnd();

public:

	//~ IAbilitySystemInterface
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	//~ End IAbilitySystemInterface

protected:

	/**
	 *  The ASC lives on the PlayerState, which arrives at a different time on
	 *  each side: the server has it by PossessedBy, a client only once
	 *  PlayerState replicates down. Both paths therefore have to initialise the
	 *  actor info, and both funnel through InitAbilityActorInfo below --
	 *  handling only one of them is the classic way to get an ASC that works in
	 *  standalone and silently does nothing in a networked game.
	 */
	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_PlayerState() override;

	/** Resolves the PlayerState's ASC, caches it, and binds it to this pawn. */
	void InitAbilityActorInfo();

	UPROPERTY(Transient)
	TObjectPtr<UARPGAbilitySystemComponent> CachedAbilitySystemComponent;

	// --- Debug harness --------------------------------------------------------
	//
	// A stand-in for the real weapon hitbox, which does not exist until phase 4
	// wires attacks to montages. Exists so the phase 1 damage pipeline can be
	// exercised end-to-end in PIE -- hitbox sweep, faction filter, execution,
	// attribute change, replication -- without waiting three phases for an
	// animation system.
	//
	// Console commands rather than input bindings: a new binding would need an
	// InputAction asset and an IMC entry, which is content authoring for
	// something that should disappear by phase 4.

	/** Arms the debug hitbox briefly. Routes through the server so it works from a client. */
	UFUNCTION(Exec)
	void ARPGSwing();

	/**
	 *  Drives the real attack path: combo component resolves the tree position and
	 *  raises Event.Attack.Begin, the melee ability plays the montage, and its
	 *  notifies arm the hitbox and open the cancel window.
	 *
	 *  Distinct from ARPGSwing, which arms the debug hitbox directly and
	 *  deliberately bypasses all of that -- keeping both means the damage pipeline
	 *  can still be tested in isolation when an attack misbehaves.
	 */
	UFUNCTION(Exec)
	void ARPGAttack();

	UFUNCTION(Exec)
	void ARPGAttackHeavy();

	/** Reports combo position and granted abilities. */
	UFUNCTION(Exec)
	void ARPGComboState();

	/** Prints this character's vitals, on whichever machine you type it. */
	UFUNCTION(Exec)
	void ARPGStats();

	/** Toggles trace visualisation for the debug hitbox. */
	UFUNCTION(Exec)
	void ARPGDebugDraw(bool bEnabled);

protected:
	UFUNCTION(Server, Reliable)
	void ServerDebugSwing();

	UFUNCTION(Server, Reliable)
	void ServerComboInput(bool bHeavy);

	/**
	 *  Granted once, server-side, when the ability actor info is initialised.
	 *
	 *  A gameplay-event-triggered ability only fires if it has been GRANTED to
	 *  the ability system component first -- raising Event.Attack.Begin against
	 *  an ASC that was never given the melee ability does nothing at all, with no
	 *  error to say so.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "ARPG|Abilities")
	TArray<TSubclassOf<UGameplayAbility>> DefaultAbilities;

	/** Assigned to the combo component on begin play. Soft, so no hard content reference. */
	UPROPERTY(EditAnywhere, Category = "ARPG|Combat")
	TSoftObjectPtr<UARPGWeaponAttackTree> DefaultAttackTree;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UARPGComboComponent> ComboComponent;

	/** The modal control scheme. Bound in SetupPlayerInputComponent. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGModalInputComponent> ModalInput;

	/** Speed tiers and the stamina that pays for the sprint. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGLocomotionComponent> Locomotion;

	/** Routes modal input events into the combat and magic components. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGPlayerActionComponent> PlayerActions;

	// The components the modal scheme routes INTO. Created here rather than
	// added per-Blueprint because the scheme is not partially useful: a control
	// layout whose magic buttons reach nothing is not a smaller feature, it is a
	// controller with dead keys. Every handler null-checks, so a derived pawn
	// may still remove any of them.

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGWeaponComponent> WeaponComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGParryComponent> ParryComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGMagicComponent> MagicComponent;

	/** Shows what is readied. Without it, LT appears to do nothing at all. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGHandVisualComponent> HandVisual;

	/** The bar the D-pad cycles. Reads its stock from the inventory below. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGQuickSlotComponent> QuickSlotComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGInventoryComponent> InventoryComponent;

	/** Toggles the sprint. */
	void ToggleSprint();

	bool bAbilitiesGranted = false;

	void EndDebugSwing();

	UPROPERTY(VisibleAnywhere, Category = "ARPG|Debug")
	TObjectPtr<UARPGHitboxComponent> DebugHitbox;

	/**
	 *  Damage type stamped onto debug swings.
	 *
	 *  Without one, UARPGDamageExecution has no categories, penetration or
	 *  resistance attribute to work from, so it warns and deals the raw number
	 *  unmitigated -- which looks correct against a target that happens to have
	 *  no armor or resistance, and is silently wrong against anything else.
	 *
	 *  A soft reference, resolved on first swing: this is the one place the
	 *  debug harness needs to name content, and a soft path keeps that out of
	 *  the constructor and non-fatal if the asset is ever moved or deleted.
	 */
	UPROPERTY(EditAnywhere, Category = "ARPG|Debug")
	TSoftObjectPtr<UARPGDamageTypeAsset> DebugDamageType;

	/** How long the debug hitbox stays armed, in seconds. */
	UPROPERTY(EditAnywhere, Category = "ARPG|Debug", meta = (ClampMin = "0.01"))
	float DebugSwingDuration = 0.2f;

	FTimerHandle DebugSwingTimer;

public:

	/** Returns CameraBoom subobject **/
	FORCEINLINE class USpringArmComponent* GetCameraBoom() const { return CameraBoom; }

	/** Returns FollowCamera subobject **/
	FORCEINLINE class UCameraComponent* GetFollowCamera() const { return FollowCamera; }
};

