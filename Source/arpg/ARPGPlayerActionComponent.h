// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGGameplayComponentBase.h"
#include "ARPGModalInputComponent.h"
#include "ARPGMagicTypes.h"
#include "GameplayTagContainer.h"
#include "ARPGPlayerActionComponent.generated.h"

class UARPGComboComponent;
class UARPGLocomotionComponent;
class UARPGMagicComponent;
class UARPGParryComponent;
class UARPGQuickSlotComponent;
class UARPGWeaponComponent;
class UAbilitySystemComponent;

/**
 * Turns modal input events into gameplay. The other half of Godot's
 * PlayerCharacter -- everything its _ic_* handlers did.
 *
 * Sits deliberately between UARPGModalInputComponent, which knows what button
 * was pressed and nothing else, and the combat and magic components, which know
 * what an action does and nothing about buttons. This is where the two meet, so
 * this is where the rules that are neither input nor combat live:
 *
 * **A readied element is spent by whatever you do next.** Elements stay in hand
 * after LT is released; the following action decides what they were for -- a
 * weapon attack imbues, a dodge becomes an elemental dodge, RT plus a face
 * button discharges. That is the central idea of the magic scheme and it cannot
 * live in any one of those systems, because it is precisely the choice BETWEEN
 * them.
 *
 * **Presses are buffered to the cancel window, not dropped.** Dodge, block and
 * element selection pressed mid-swing fire the instant the attack becomes
 * cancellable. Without this the player presses, sees nothing, presses again, and
 * gets two dodges out the far side of the animation.
 *
 * The one input NOT buffered is drinking a potion. A potion that goes down
 * several beats late -- after the swing finally ends, at the moment the player
 * has given up and pressed something else -- is worse than one that plainly did
 * not happen. Cycling the bar stays available throughout, because choosing is
 * not drinking.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class UARPGPlayerActionComponent : public UARPGGameplayComponentBase
{
	GENERATED_BODY()

public:
	UARPGPlayerActionComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/** Subscribes to a modal input component's events. Idempotent. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Player")
	void BindInput(UARPGModalInputComponent* Input);

	/**
	 * Seconds to wait after starting a draw before a buffered block lands.
	 *
	 * LB while sheathed draws INTO a block rather than blocking bare-handed, and
	 * the guard has to come up when the weapon is actually in hand -- otherwise
	 * the character parries with an empty fist for the length of the draw.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Player", meta = (ClampMin = "0.0"))
	float BlockDrawDuration = 0.35f;

	UFUNCTION(BlueprintPure, Category = "ARPG|Player")
	bool IsBlockPending() const { return bBlockPending; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Player")
	bool IsDodgePending() const { return bDodgePending; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Player")
	int32 GetPendingMagicSelectMask() const { return PendingMagicSelectMask; }

	// --- Modal input handlers ---------------------------------------------------
	//
	// UFUNCTIONs because the modal input component's events are dynamic
	// delegates. Public so a test can drive them without an input component.

	UFUNCTION() void HandleJump();
	UFUNCTION() void HandleDodge();
	UFUNCTION() void HandleLightAttack();
	UFUNCTION() void HandleHeavyAttack();
	UFUNCTION() void HandleSpecialAttack();
	UFUNCTION() void HandleLightAttackReleased();
	UFUNCTION() void HandleHeavyAttackReleased();
	UFUNCTION() void HandleSpecialAttackReleased();
	UFUNCTION() void HandleParryPressed();
	UFUNCTION() void HandleParryReleased();
	UFUNCTION() void HandleSheathe();
	UFUNCTION() void HandleMagicSelect(EARPGInputFace Slot);
	UFUNCTION() void HandleMagicDiscard();
	UFUNCTION() void HandleMagicPagePrev();
	UFUNCTION() void HandleMagicPageNext();
	UFUNCTION() void HandleQuickSlotPrev();
	UFUNCTION() void HandleQuickSlotNext();
	UFUNCTION() void HandleQuickSlotUse();
	UFUNCTION() void HandleDischargeModifierPressed();
	UFUNCTION() void HandleDischargeChargeStarted(EARPGInputFace Slot);
	UFUNCTION() void HandleDischargeActivated(EARPGInputFace Slot, float Charge);
	UFUNCTION() void HandleDischargeCancelled();

	/**
	 * Which delivery a face button asks for while RT is held.
	 *
	 * The mapping is authored on EARPGDischargeType itself, which is why this is
	 * a switch rather than a configurable table: changing it would make the
	 * enum's own documentation wrong.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Player")
	static EARPGDischargeType FaceToDischargeType(EARPGInputFace Slot);

	/** The ability tag for the delivery a face button asks for. */
	static FGameplayTag FaceToDischargeAbilityTag(EARPGInputFace Slot);

protected:
	UARPGComboComponent* GetCombo() const;
	UARPGWeaponComponent* GetWeapon() const;
	UARPGParryComponent* GetParry() const;
	UARPGMagicComponent* GetMagic() const;
	UARPGQuickSlotComponent* GetQuickSlots() const;
	UARPGLocomotionComponent* GetLocomotion() const;

	bool IsDead() const;
	bool IsDodging() const;
	bool IsAttackActive() const;

	/** Equipped and out. Attacks refuse while it is equipped but sheathed. */
	bool HasWeaponEquipped() const;

	/** Shared by light, heavy and special: the gates, the block exit, the imbue. */
	void RouteAttack(EARPGInputFace Slot, bool bSpecial);

	/** Starts the block if the parry component and weapon allow it. */
	void TryBeginBlock();

	/** Fires the dodge ability -- elemental or standard, the ability decides. */
	void TriggerDodge();

	/** Pipes an input-release into every active ability carrying the tag. */
	void ReleaseAbilityInput(FGameplayTag AbilityTag);

private:
	/** Keeps the locomotion component's walk lock in step with what is readied. */
	void SyncWalkForced();

	/** Holds off auto-sheathe while any NPC still has this actor as its target. */
	void SyncAutoSheatheSuppression();

	/**
	 * Resolves and caches every sibling this component drives.
	 *
	 * The six accessors below each ran FindComponentByClass -- a walk of the
	 * owner's whole component array -- and the tick alone triggered several per
	 * frame. Sibling components do not come and go on a possessed pawn, so they
	 * are resolved once.
	 *
	 * ON FIRST USE, not only at BeginPlay. An input event can arrive before begin
	 * play in a world that has not started one -- an automation fixture, a tools
	 * harness -- and caching at BeginPlay alone left every accessor returning null
	 * there, so nothing this component drives did anything at all.
	 */
	void EnsureSiblings() const;

	UPROPERTY(Transient)
	TObjectPtr<UARPGModalInputComponent> BoundInput;

	mutable bool bSiblingsCached = false;

	UPROPERTY(Transient)
	mutable TObjectPtr<UARPGComboComponent> CachedCombo;

	UPROPERTY(Transient)
	mutable TObjectPtr<UARPGWeaponComponent> CachedWeapon;

	UPROPERTY(Transient)
	mutable TObjectPtr<UARPGParryComponent> CachedParry;

	UPROPERTY(Transient)
	mutable TObjectPtr<UARPGMagicComponent> CachedMagic;

	UPROPERTY(Transient)
	mutable TObjectPtr<UARPGQuickSlotComponent> CachedQuickSlots;

	UPROPERTY(Transient)
	mutable TObjectPtr<UARPGLocomotionComponent> CachedLocomotion;

	/** LB is held. Distinct from "blocking": the guard may not be up yet. */
	bool bParryHeld = false;

	bool bBlockPending = false;
	bool bDodgePending = false;

	/** One bit per element slot, replayed when the cancel window opens. */
	int32 PendingMagicSelectMask = 0;

	bool bDrawingForBlock = false;
	float DrawWaitElapsed = 0.f;
};
