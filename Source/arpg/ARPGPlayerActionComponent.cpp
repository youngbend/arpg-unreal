// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGPlayerActionComponent.h"
#include "ARPGComboComponent.h"
#include "ARPGGameplayTags.h"
#include "ARPGLocomotionComponent.h"
#include "ARPGMagicComponent.h"
#include "ARPGMagicLoadout.h"
#include "ARPGParryComponent.h"
#include "ARPGQuickSlotComponent.h"
#include "ARPGReactionDefinitions.h"
#include "ARPGWeaponAttackTree.h"
#include "ARPGWeaponComponent.h"
#include "ARPGWeaponDefinition.h"
#include "ARPGThreatRegistry.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"

UARPGPlayerActionComponent::UARPGPlayerActionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UARPGPlayerActionComponent::BeginPlay()
{
	Super::BeginPlay();

	EnsureSiblings();

	// Auto-bind to a sibling if there is one, so the common case needs no setup.
	if (!BoundInput)
	{
		if (AActor* Owner = GetOwner())
		{
			BindInput(Owner->FindComponentByClass<UARPGModalInputComponent>());
		}
	}
}

void UARPGPlayerActionComponent::BindInput(UARPGModalInputComponent* Input)
{
	if (!Input || Input == BoundInput)
	{
		return;
	}

	BoundInput = Input;

	Input->OnJump.AddDynamic(this, &UARPGPlayerActionComponent::HandleJump);
	Input->OnDodge.AddDynamic(this, &UARPGPlayerActionComponent::HandleDodge);

	Input->OnLightAttack.AddDynamic(this, &UARPGPlayerActionComponent::HandleLightAttack);
	Input->OnHeavyAttack.AddDynamic(this, &UARPGPlayerActionComponent::HandleHeavyAttack);
	Input->OnSpecialAttack.AddDynamic(this, &UARPGPlayerActionComponent::HandleSpecialAttack);
	Input->OnLightAttackReleased.AddDynamic(this, &UARPGPlayerActionComponent::HandleLightAttackReleased);
	Input->OnHeavyAttackReleased.AddDynamic(this, &UARPGPlayerActionComponent::HandleHeavyAttackReleased);
	Input->OnSpecialAttackReleased.AddDynamic(this, &UARPGPlayerActionComponent::HandleSpecialAttackReleased);

	Input->OnParryPressed.AddDynamic(this, &UARPGPlayerActionComponent::HandleParryPressed);
	Input->OnParryReleased.AddDynamic(this, &UARPGPlayerActionComponent::HandleParryReleased);
	Input->OnSheathePressed.AddDynamic(this, &UARPGPlayerActionComponent::HandleSheathe);

	Input->OnMagicSelect.AddDynamic(this, &UARPGPlayerActionComponent::HandleMagicSelect);
	Input->OnMagicDiscard.AddDynamic(this, &UARPGPlayerActionComponent::HandleMagicDiscard);
	Input->OnMagicPagePrev.AddDynamic(this, &UARPGPlayerActionComponent::HandleMagicPagePrev);
	Input->OnMagicPageNext.AddDynamic(this, &UARPGPlayerActionComponent::HandleMagicPageNext);

	Input->OnQuickSlotPrev.AddDynamic(this, &UARPGPlayerActionComponent::HandleQuickSlotPrev);
	Input->OnQuickSlotNext.AddDynamic(this, &UARPGPlayerActionComponent::HandleQuickSlotNext);
	Input->OnQuickSlotUse.AddDynamic(this, &UARPGPlayerActionComponent::HandleQuickSlotUse);

	Input->OnDischargeModifierPressed.AddDynamic(this,
		&UARPGPlayerActionComponent::HandleDischargeModifierPressed);
	Input->OnDischargeChargeStarted.AddDynamic(this,
		&UARPGPlayerActionComponent::HandleDischargeChargeStarted);
	Input->OnDischargeActivated.AddDynamic(this, &UARPGPlayerActionComponent::HandleDischargeActivated);
	Input->OnDischargeCancelled.AddDynamic(this, &UARPGPlayerActionComponent::HandleDischargeCancelled);
}

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------

void UARPGPlayerActionComponent::EnsureSiblings() const
{
	if (bSiblingsCached)
	{
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return; // not cached: retried once there is an owner to search
	}

	bSiblingsCached = true;

	CachedCombo = Owner->FindComponentByClass<UARPGComboComponent>();
	CachedWeapon = Owner->FindComponentByClass<UARPGWeaponComponent>();
	CachedParry = Owner->FindComponentByClass<UARPGParryComponent>();
	CachedMagic = Owner->FindComponentByClass<UARPGMagicComponent>();
	CachedQuickSlots = Owner->FindComponentByClass<UARPGQuickSlotComponent>();
	CachedLocomotion = Owner->FindComponentByClass<UARPGLocomotionComponent>();
}

UAbilitySystemComponent* UARPGPlayerActionComponent::GetASC() const
{
	// Retried while null rather than latched with the rest: a player's ability
	// system lives on the PlayerState, which may not have replicated in yet.
	if (!CachedASC)
	{
		CachedASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetOwner());
	}
	return CachedASC;
}

UARPGComboComponent* UARPGPlayerActionComponent::GetCombo() const
{
	EnsureSiblings();
	return CachedCombo;
}

UARPGWeaponComponent* UARPGPlayerActionComponent::GetWeapon() const
{
	EnsureSiblings();
	return CachedWeapon;
}

UARPGParryComponent* UARPGPlayerActionComponent::GetParry() const
{
	EnsureSiblings();
	return CachedParry;
}

UARPGMagicComponent* UARPGPlayerActionComponent::GetMagic() const
{
	EnsureSiblings();
	return CachedMagic;
}

UARPGQuickSlotComponent* UARPGPlayerActionComponent::GetQuickSlots() const
{
	EnsureSiblings();
	return CachedQuickSlots;
}

UARPGLocomotionComponent* UARPGPlayerActionComponent::GetLocomotion() const
{
	EnsureSiblings();
	return CachedLocomotion;
}

bool UARPGPlayerActionComponent::IsDead() const
{
	const UAbilitySystemComponent* ASC = GetASC();
	return ASC && ASC->HasMatchingGameplayTag(TAG_State_Dead);
}

bool UARPGPlayerActionComponent::IsDodging() const
{
	const UAbilitySystemComponent* ASC = GetASC();
	return ASC && ASC->HasMatchingGameplayTag(TAG_State_Dodging);
}

bool UARPGPlayerActionComponent::IsAttackActive() const
{
	const UARPGComboComponent* Combo = GetCombo();
	return Combo && Combo->IsAttacking();
}

bool UARPGPlayerActionComponent::HasWeaponEquipped() const
{
	const UARPGWeaponComponent* Weapon = GetWeapon();
	return Weapon && Weapon->GetWeapon() != nullptr;
}

// ---------------------------------------------------------------------------
// Tick -- buffer flushing
// ---------------------------------------------------------------------------

void UARPGPlayerActionComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	SyncWalkForced();
	SyncAutoSheatheSuppression();

	if (bDrawingForBlock)
	{
		DrawWaitElapsed += DeltaTime;
		if (DrawWaitElapsed >= BlockDrawDuration)
		{
			bDrawingForBlock = false;
		}
	}

	const bool bCancelWindowOpen = !IsAttackActive();

	if (bDodgePending && bCancelWindowOpen)
	{
		bDodgePending = false;
		HandleDodge();
	}

	// Replayed as a mask rather than a queue: ToggleElement no-ops on a slot that
	// is already active, so replaying a slot the player toggled twice mid-swing
	// cannot land them on the opposite state from what they see readied.
	if (PendingMagicSelectMask != 0 && bCancelWindowOpen)
	{
		const int32 Mask = PendingMagicSelectMask;
		PendingMagicSelectMask = 0;

		if (UARPGMagicComponent* Magic = GetMagic())
		{
			for (int32 Slot = 0; Slot < UARPGMagicLoadout::SlotCount; ++Slot)
			{
				if (Mask & (1 << Slot))
				{
					Magic->ToggleElement(static_cast<EARPGElementSlot>(Slot));
				}
			}

			if (UARPGComboComponent* Combo = GetCombo())
			{
				Combo->KeepAlive();
			}
		}
	}

	if (bBlockPending && bCancelWindowOpen && !bDrawingForBlock && !IsDodging())
	{
		bBlockPending = false;
		TryBeginBlock();
	}
}

void UARPGPlayerActionComponent::SyncWalkForced()
{
	UARPGLocomotionComponent* Locomotion = GetLocomotion();
	if (!Locomotion)
	{
		return;
	}

	const UARPGMagicComponent* Magic = GetMagic();
	Locomotion->SetWalkForced(Magic && Magic->HasActiveElements());
}

void UARPGPlayerActionComponent::SyncAutoSheatheSuppression()
{
	UARPGWeaponComponent* Weapon = GetWeapon();
	if (!Weapon || !Weapon->IsDrawn())
	{
		return;
	}

	const AActor* Owner = GetOwner();
	const UWorld* World = GetWorld();
	if (!Owner || !World)
	{
		return;
	}

	// A map lookup. This used to iterate every actor in the level looking for AI
	// controllers and read each one's blackboard, throttled to twice a second to
	// make the cost bearable -- so the answer also lagged by up to half a second.
	// Perception now pushes each transition as it happens, so this is both exact
	// and cheap enough to ask every frame.
	//
	// Still the COMMITTED target rather than merely perceived: the registry is
	// fed from UARPGPerceptionComponent::SetTarget, which is the same commitment
	// the behaviour tree acts on.
	if (const UARPGThreatRegistry* Threats = World->GetSubsystem<UARPGThreatRegistry>())
	{
		Weapon->SetAutoSheatheSuppressed(Threats->IsTargeted(Owner));
	}
}

// ---------------------------------------------------------------------------
// Movement actions
// ---------------------------------------------------------------------------

void UARPGPlayerActionComponent::HandleJump()
{
	if (IsDead())
	{
		return;
	}

	if (ACharacter* Character = Cast<ACharacter>(GetOwner()))
	{
		Character->Jump();
	}
}

void UARPGPlayerActionComponent::HandleDodge()
{
	if (IsDead())
	{
		return;
	}

	if (IsAttackActive())
	{
		bDodgePending = true;
		return;
	}

	TriggerDodge();
}

void UARPGPlayerActionComponent::TriggerDodge()
{
	// One ability for both the standard and the elemental dodge; it consumes a
	// readied element itself and picks accordingly. Nothing here needs to know
	// which one the player is about to get -- see UARPGGameplayAbility_Dodge.
	if (UAbilitySystemComponent* ASC = GetASC())
	{
		ASC->TryActivateAbilitiesByTag(FGameplayTagContainer(TAG_Ability_Dodge));
	}
}

// ---------------------------------------------------------------------------
// Attacks
// ---------------------------------------------------------------------------

void UARPGPlayerActionComponent::HandleLightAttack() { RouteAttack(EARPGInputFace::West, false); }
void UARPGPlayerActionComponent::HandleHeavyAttack() { RouteAttack(EARPGInputFace::North, false); }
void UARPGPlayerActionComponent::HandleSpecialAttack() { RouteAttack(EARPGInputFace::None, true); }

void UARPGPlayerActionComponent::RouteAttack(EARPGInputFace Slot, bool bSpecial)
{
	if (IsDead() || IsDodging())
	{
		return;
	}

	UARPGWeaponComponent* Weapon = GetWeapon();

	// A weapon that is equipped but sheathed cannot swing. RB and LB draw it;
	// the light and heavy buttons simply refuse, so a player who has put their
	// sword away never fires a bare-handed swing they did not ask for.
	if (Weapon && Weapon->GetWeapon() && !Weapon->IsDrawn())
	{
		if (!bSpecial)
		{
			return;
		}

		// RB while sheathed draws to attack-ready and CONSUMES the press. Drawing
		// and swinging off one button would make the first special after
		// sheathing a different move from every one after it.
		Weapon->SetDrawn(true);
		return;
	}

	UARPGParryComponent* Parry = GetParry();
	const bool bEmpowered = Parry && Parry->ConsumeEmpowered();

	// Attacking leaves the guard, and clears the held-block intent with it --
	// otherwise the buffer flush above would raise the guard again the moment
	// the swing became cancellable, mid-combo.
	if (Parry && Parry->IsBlocking())
	{
		Parry->EndBlock();

		// The block's movement scaling goes with it. The attack sets its own a
		// moment later; leaving the block's in place would have the first swing
		// out of a guard move at whatever the shield allowed.
		if (UARPGLocomotionComponent* Locomotion = GetLocomotion())
		{
			Locomotion->ClearAttackMovement();
		}
	}
	bParryHeld = false;
	bBlockPending = false;

	if (Weapon)
	{
		Weapon->NotifyCombatActivity();
	}

	if (UARPGComboComponent* Combo = GetCombo())
	{
		EARPGAttackInput Input = EARPGAttackInput::Light;
		if (bSpecial)
		{
			Input = EARPGAttackInput::Special;
		}
		else if (Slot == EARPGInputFace::North)
		{
			Input = EARPGAttackInput::Heavy;
		}

		Combo->ReceiveInput(Input, bEmpowered);
	}

	// The readied element is spent by the swing. Fires after the combo input so
	// the imbue lands on the attack that was just started rather than the one
	// before it; the ability itself no-ops when nothing is readied.
	if (UAbilitySystemComponent* ASC = GetASC())
	{
		ASC->TryActivateAbilitiesByTag(FGameplayTagContainer(TAG_Ability_Imbue));
	}
}

void UARPGPlayerActionComponent::HandleLightAttackReleased()
{
	if (UARPGComboComponent* Combo = GetCombo())
	{
		Combo->ReceiveInputReleased(EARPGAttackInput::Light);
	}
}

void UARPGPlayerActionComponent::HandleHeavyAttackReleased()
{
	if (UARPGComboComponent* Combo = GetCombo())
	{
		Combo->ReceiveInputReleased(EARPGAttackInput::Heavy);
	}
}

void UARPGPlayerActionComponent::HandleSpecialAttackReleased()
{
	if (UARPGComboComponent* Combo = GetCombo())
	{
		Combo->ReceiveInputReleased(EARPGAttackInput::Special);
	}
}

// ---------------------------------------------------------------------------
// Block
// ---------------------------------------------------------------------------

void UARPGPlayerActionComponent::HandleParryPressed()
{
	if (IsDead())
	{
		return;
	}

	bParryHeld = true;
	bBlockPending = true;

	// LB while sheathed draws INTO the block. The guard then waits out the draw
	// rather than snapping up over an empty hand -- see BlockDrawDuration.
	UARPGWeaponComponent* Weapon = GetWeapon();
	const bool bNeedsDraw = Weapon && HasWeaponEquipped() && !Weapon->IsDrawn();
	if (bNeedsDraw)
	{
		Weapon->SetDrawn(true);
		bDrawingForBlock = true;
		DrawWaitElapsed = 0.f;
		return;
	}

	if (!IsDodging() && !IsAttackActive())
	{
		bBlockPending = false;
		TryBeginBlock();
	}
}

void UARPGPlayerActionComponent::HandleParryReleased()
{
	bParryHeld = false;
	bBlockPending = false;

	if (UARPGParryComponent* Parry = GetParry())
	{
		Parry->EndBlock();
	}

	// Give the movement scale back. Without this the character walks at block
	// speed for the rest of their life, and it reads as a movement bug rather
	// than as a guard nobody dropped.
	if (UARPGLocomotionComponent* Locomotion = GetLocomotion())
	{
		Locomotion->ClearAttackMovement();
	}
}

void UARPGPlayerActionComponent::TryBeginBlock()
{
	// Re-checked rather than assumed: this runs from the tick flush as well as
	// from the press, and LB may have come up in between.
	UARPGParryComponent* Parry = GetParry();
	if (!Parry || !bParryHeld || IsDead() || IsDodging())
	{
		return;
	}

	// Blocking is a property of the weapon, not of the character: there is
	// nothing to raise bare-handed. A weapon with no attack tree has no moveset
	// at all, which counts as nothing to raise for the same reason.
	UARPGWeaponComponent* Weapon = GetWeapon();
	const UARPGWeaponDefinition* Definition = Weapon ? Weapon->GetWeapon() : nullptr;
	const UARPGWeaponAttackTree* Tree = Definition ? Definition->AttackTree : nullptr;
	if (!Tree)
	{
		return;
	}

	// The WEAPON's timings, so a buckler that snaps up with a narrow parry and a
	// greatshield that takes a beat and then covers everything genuinely differ.
	// A tree with no block authored falls back to the parry component's own
	// settings rather than refusing -- an unfinished weapon should still defend.
	if (const UARPGBlockDefinition* Block = Tree->Block)
	{
		Parry->BlendInTime = Block->BlendInTime;
		Parry->ParryWindow = Block->ParryWindow;

		// Holding a guard costs mobility, and how much is part of what
		// distinguishes the weapon. Sprint stays permitted: it is the movement
		// SCALE that a block restricts, and dropping the guard to run is the
		// player's call, not the block's.
		if (UARPGLocomotionComponent* Locomotion = GetLocomotion())
		{
			Locomotion->SetAttackMovement(Block->MovementSpeedFactor, /*bAllowSprint=*/true);
		}
	}

	Parry->BeginBlock();
	Weapon->NotifyCombatActivity();
}

void UARPGPlayerActionComponent::HandleSheathe()
{
	UARPGWeaponComponent* Weapon = GetWeapon();
	if (Weapon && Weapon->IsDrawn() && !IsAttackActive())
	{
		Weapon->SetDrawn(false);
	}
}

// ---------------------------------------------------------------------------
// Magic
// ---------------------------------------------------------------------------

void UARPGPlayerActionComponent::HandleMagicSelect(EARPGInputFace Slot)
{
	// Nothing is readied while dead. Input still arrives during the death
	// window, and an element readied there would survive into the respawn.
	if (IsDead() || Slot == EARPGInputFace::None)
	{
		return;
	}

	const int32 Index = static_cast<int32>(Slot);

	if (IsAttackActive())
	{
		PendingMagicSelectMask |= (1 << Index);
		return;
	}

	if (UARPGMagicComponent* Magic = GetMagic())
	{
		Magic->ToggleElement(static_cast<EARPGElementSlot>(Index));
	}

	// Readying an element keeps the chain alive without extending it: the reset
	// timer restarts, but the combo does not advance. Mixing elements mid-combo
	// is meant to be possible without the chain dying under the player's hands.
	if (UARPGComboComponent* Combo = GetCombo())
	{
		Combo->KeepAlive();
	}
}

void UARPGPlayerActionComponent::HandleMagicDiscard()
{
	if (IsDead())
	{
		return;
	}

	if (UARPGMagicComponent* Magic = GetMagic())
	{
		Magic->ClearSelection();
	}
}

void UARPGPlayerActionComponent::HandleMagicPagePrev()
{
	if (IsDead())
	{
		return;
	}

	UARPGMagicComponent* Magic = GetMagic();
	const UARPGMagicLoadout* Loadout = Magic ? Magic->Loadout : nullptr;
	if (!Loadout || Loadout->PageCount <= 0)
	{
		return;
	}

	// Wraps, so paging is a cycle rather than a bounded list the player has to
	// remember the ends of.
	const int32 Count = Loadout->PageCount;
	Magic->SetCurrentPage(((Magic->GetCurrentPage() - 1) % Count + Count) % Count);
}

void UARPGPlayerActionComponent::HandleMagicPageNext()
{
	if (IsDead())
	{
		return;
	}

	UARPGMagicComponent* Magic = GetMagic();
	const UARPGMagicLoadout* Loadout = Magic ? Magic->Loadout : nullptr;
	if (!Loadout || Loadout->PageCount <= 0)
	{
		return;
	}

	const int32 Count = Loadout->PageCount;
	Magic->SetCurrentPage((Magic->GetCurrentPage() + 1) % Count);
}

// ---------------------------------------------------------------------------
// Discharge
// ---------------------------------------------------------------------------

EARPGDischargeType UARPGPlayerActionComponent::FaceToDischargeType(EARPGInputFace Slot)
{
	switch (Slot)
	{
	case EARPGInputFace::North: return EARPGDischargeType::Project;  // Y -- mid-range projectile
	case EARPGInputFace::West:  return EARPGDischargeType::Burst;    // X -- close cone
	case EARPGInputFace::South: return EARPGDischargeType::Emanate;  // A -- around the caster
	case EARPGInputFace::East:  return EARPGDischargeType::Cloak;    // B -- onto the caster
	default: return EARPGDischargeType::Project;
	}
}

void UARPGPlayerActionComponent::HandleDischargeModifierPressed()
{
	if (IsDead())
	{
		return;
	}

	// RT with nothing readied means "give me back what I just cast". Repeating a
	// two-element combination is otherwise four button presses every time.
	UARPGMagicComponent* Magic = GetMagic();
	if (Magic && !Magic->HasActiveElements())
	{
		Magic->BeginAutoReady();
	}
}

FGameplayTag UARPGPlayerActionComponent::FaceToDischargeAbilityTag(EARPGInputFace Slot)
{
	switch (FaceToDischargeType(Slot))
	{
	case EARPGDischargeType::Burst:   return TAG_Ability_Discharge_Burst;
	case EARPGDischargeType::Emanate: return TAG_Ability_Discharge_Emanate;
	case EARPGDischargeType::Cloak:   return TAG_Ability_Discharge_Cloak;
	default: return TAG_Ability_Discharge_Project;
	}
}

void UARPGPlayerActionComponent::HandleDischargeChargeStarted(EARPGInputFace Slot)
{
	if (IsDead() || Slot == EARPGInputFace::None)
	{
		return;
	}

	// Activated on the PRESS, so the ability's charge task runs for the whole
	// hold: it is what plays the wind-up, spends mana as it accumulates, and
	// forces the release when the player runs dry. Activating on the release
	// instead would compress every charge into a single frame at minimum
	// strength, which is the same thing as not having charging at all.
	//
	// The ability itself handles the nothing-readied case by auto-readying, and
	// the no-recipe case by failing the spell without eating the elements -- so
	// this deliberately does not pre-check either.
	if (UAbilitySystemComponent* ASC = GetASC())
	{
		ASC->TryActivateAbilitiesByTag(FGameplayTagContainer(FaceToDischargeAbilityTag(Slot)));
	}
}

void UARPGPlayerActionComponent::HandleDischargeActivated(EARPGInputFace Slot, float Charge)
{
	if (Slot == EARPGInputFace::None)
	{
		return;
	}

	// NOT gated on being alive, and that is deliberate: an ability charging when
	// the character died still has to be told to let go, or its task keeps
	// draining mana against a corpse.
	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC)
	{
		return;
	}

	// Letting go IS the cast. Routed as an input-release rather than as a fresh
	// activation because the ability is already running -- its charge task is
	// the only thing that knows how much was actually paid for, which is why
	// the Charge reported here is not passed on. See OnDischargeActivated.
	const FGameplayTag AbilityTag = FaceToDischargeAbilityTag(Slot);
	ReleaseAbilityInput(AbilityTag);

	(void)Charge;

	// Releasing a discharge breaks the melee chain exactly as a weapon attack
	// would: the two are alternatives, not layers.
	if (UARPGComboComponent* Combo = GetCombo())
	{
		Combo->ResetCombo();
	}
}

void UARPGPlayerActionComponent::ReleaseAbilityInput(FGameplayTag AbilityTag)
{
	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC)
	{
		return;
	}

	// Handles are collected first: AbilitySpecInputReleased runs ability code
	// that can end the ability, and ending an ability may reallocate the
	// activatable list out from under an iterator walking it.
	TArray<FGameplayAbilitySpecHandle> Handles;
	for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
	{
		if (Spec.IsActive() && Spec.Ability && Spec.Ability->GetAssetTags().HasTag(AbilityTag))
		{
			Handles.Add(Spec.Handle);
		}
	}

	for (const FGameplayAbilitySpecHandle& Handle : Handles)
	{
		if (FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(Handle))
		{
			ASC->AbilitySpecInputReleased(*Spec);
		}
	}
}

void UARPGPlayerActionComponent::HandleDischargeCancelled()
{
	if (UAbilitySystemComponent* ASC = GetASC())
	{
		// By the parent tag, so all four deliveries are covered at once.
		FGameplayTagContainer DischargeTags(TAG_Ability_Discharge);
		ASC->CancelAbilities(&DischargeTags);
	}
}

// ---------------------------------------------------------------------------
// Quick slots
// ---------------------------------------------------------------------------

void UARPGPlayerActionComponent::HandleQuickSlotPrev()
{
	// Cycling is a selection change, not a use, so it stays available mid-swing.
	if (IsDead())
	{
		return;
	}

	if (UARPGQuickSlotComponent* QuickSlots = GetQuickSlots())
	{
		QuickSlots->CycleSelection(-1);
	}
}

void UARPGPlayerActionComponent::HandleQuickSlotNext()
{
	if (IsDead())
	{
		return;
	}

	if (UARPGQuickSlotComponent* QuickSlots = GetQuickSlots())
	{
		QuickSlots->CycleSelection(1);
	}
}

void UARPGPlayerActionComponent::HandleQuickSlotUse()
{
	// DROPPED rather than buffered, unlike the dodge and the block. See the
	// class comment: a potion that arrives late is worse than one that did not
	// arrive at all.
	if (IsDead() || IsAttackActive() || IsDodging())
	{
		return;
	}

	if (UARPGQuickSlotComponent* QuickSlots = GetQuickSlots())
	{
		QuickSlots->UseSelected();
	}
}
