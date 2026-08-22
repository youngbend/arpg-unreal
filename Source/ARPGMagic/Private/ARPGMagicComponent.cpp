// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGMagicComponent.h"
#include "ARPGAttributeLibrary.h"
#include "ARPGMagic.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicElement.h"
#include "ARPGMagicProgression.h"
#include "ARPGOffenseSet.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"
#include "Net/UnrealNetwork.h"

UARPGMagicComponent::UARPGMagicComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	SetIsReplicatedByDefault(true);
}

void UARPGMagicComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// To everyone, not just the owner: a readied element is visible in the
	// caster's hand, so other players need the mask to show it.
	DOREPLIFETIME(UARPGMagicComponent, ActiveMask);
	DOREPLIFETIME(UARPGMagicComponent, CurrentPage);
}

void UARPGMagicComponent::BeginPlay()
{
	Super::BeginPlay();

	if (Loadout)
	{
		Loadout->ConformToPageCount();
	}
}

float UARPGMagicComponent::GetAvailableMana() const
{
	const UAbilitySystemComponent* ASC = GetASC();

	// Must match TrySpendMana's treatment of a missing ability system -- see the
	// header. Reporting 0 here while spending succeeded would force every charge
	// to release on its first tick.
	return ASC ? ASC->GetNumericAttribute(UARPGVitalSet::GetManaAttribute())
	           : TNumericLimits<float>::Max();
}

bool UARPGMagicComponent::TrySpendMana(float Cost)
{
	// Shared helper. No ability system means no mana pool, which it treats as
	// infinite rather than as zero -- that is what lets the magic system be
	// driven in isolation by a fixture or an NPC with no resources wired up, and
	// it must agree with GetAvailableMana above.
	return UARPGAttributeLibrary::TrySpend(GetASC(), UARPGVitalSet::GetManaAttribute(), Cost);
}

float UARPGMagicComponent::GetDamageAmpMultiplier() const
{
	const UAbilitySystemComponent* ASC = GetASC();
	if (!ASC)
	{
		return 1.f;
	}

	const float Amp = ASC->GetNumericAttribute(UARPGOffenseSet::GetDamageAmpMultiplierAttribute());
	return Amp > 0.f ? Amp : 1.f;
}

UObject* UARPGMagicComponent::GetProgressionProvider() const
{
	if (bSearchedProgressionProvider)
	{
		return CachedProgressionProvider;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return nullptr; // not searched: retry once there is an owner
	}

	bSearchedProgressionProvider = true;

	// Components first: the real tracker is one, and an actor that also happens
	// to implement the interface (a test caster) should not shadow it.
	//
	// GetComponents on the templated overload avoids the intermediate array of
	// every component the owner has.
	for (UActorComponent* Component : Owner->GetComponents())
	{
		if (Component && Component->Implements<UARPGMagicProgression>())
		{
			CachedProgressionProvider = Component;
			return CachedProgressionProvider;
		}
	}

	CachedProgressionProvider = Owner->Implements<UARPGMagicProgression>() ? Owner : nullptr;
	return CachedProgressionProvider;
}

float UARPGMagicComponent::GetEffectiveLevel(const UARPGMagicElement* Element) const
{
	UObject* Provider = Element ? GetProgressionProvider() : nullptr;
	if (!Provider)
	{
		return 0.f;
	}

	return IARPGMagicProgression::Execute_GetEffectiveElementLevel(Provider, Element->ElementTag);
}

float UARPGMagicComponent::GetMasteryDamageMultiplier(const UARPGMagicElement* Element) const
{
	UObject* Provider = Element ? GetProgressionProvider() : nullptr;
	if (!Provider)
	{
		return 1.f;
	}

	return IARPGMagicProgression::Execute_GetElementDamageMultiplier(Provider, Element->ElementTag);
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

bool UARPGMagicComponent::IsSlotActive(EARPGElementSlot Slot) const
{
	const int32 Index = static_cast<int32>(Slot);
	return Index >= 0 && Index < UARPGMagicLoadout::SlotCount && (ActiveMask & (1 << Index)) != 0;
}

TArray<UARPGMagicElement*> UARPGMagicComponent::GetActiveElements() const
{
	return Loadout ? Loadout->GetActiveElements(CurrentPage, ActiveMask)
	               : TArray<UARPGMagicElement*>();
}

FGameplayTagContainer UARPGMagicComponent::GetActiveElementTags() const
{
	return Loadout ? Loadout->GetActiveElementTags(CurrentPage, ActiveMask)
	               : FGameplayTagContainer();
}

UARPGMagicElement* UARPGMagicComponent::GetResolvedCombination() const
{
	if (bCombinationCacheValid)
	{
		return CachedCombination.Get();
	}

	bCombinationCacheValid = true;
	CachedCombination = nullptr;

	// One element is not a combination -- resolving it would let a single-tag row
	// fire the moment that element is readied on its own.
	if (!CombinationTable || GetActiveCount() < 2)
	{
		return nullptr;
	}

	// The expensive part: a tag-container comparison against every row in the
	// table, plus the temporary container GetActiveElementTags builds. Cached
	// until the mask or the page moves.
	CachedCombination = CombinationTable->Resolve(GetActiveElementTags(), EARPGCombinationScope::Hand);
	return CachedCombination.Get();
}

UARPGMagicElement* UARPGMagicComponent::GetDisplayElement() const
{
	if (UARPGMagicElement* Combination = GetResolvedCombination())
	{
		return Combination;
	}

	const TArray<UARPGMagicElement*> Active = GetActiveElements();
	return Active.Num() > 0 ? Active[0] : nullptr;
}

bool UARPGMagicComponent::CanDischarge() const
{
	if (!HasActiveElements())
	{
		return false;
	}

	// Two elements the caster cannot actually fuse is a failed spell, not a
	// two-element one. Checked here so both the abilities and the UI ask the
	// same question rather than each deciding for itself.
	return GetActiveCount() < 2 || GetResolvedCombination() != nullptr;
}

bool UARPGMagicComponent::PassesComplexityGate(const UARPGMagicElement* Candidate) const
{
	if (!Candidate)
	{
		return true;
	}

	if (bIgnoreComplexityGate)
	{
		return true; // testing a recipe without earning it first -- see the header
	}

	if (!GetProgressionProvider())
	{
		return true; // no progression wired up = gating disabled, e.g. an NPC
	}

	const float CandidateLevel = GetEffectiveLevel(Candidate);

	for (const UARPGMagicElement* Other : GetActiveElements())
	{
		if (!Other)
		{
			continue;
		}

		// Both directions. Holding a simple element does not license a complex
		// one, and a master of the complex one still has to know the simple one.
		if (CandidateLevel < static_cast<float>(Other->Complexity))
		{
			return false;
		}
		if (GetEffectiveLevel(Other) < static_cast<float>(Candidate->Complexity))
		{
			return false;
		}
	}

	return true;
}

void UARPGMagicComponent::ToggleElement(EARPGElementSlot Slot)
{
	if (GetOwner() && !GetOwner()->HasAuthority())
	{
		ServerToggleElement(Slot);
		return;
	}

	ActivateSlot(Slot, /*bFromPlayer=*/true);
}

void UARPGMagicComponent::ServerToggleElement_Implementation(EARPGElementSlot Slot)
{
	ActivateSlot(Slot, /*bFromPlayer=*/true);
}

void UARPGMagicComponent::ActivateSlot(EARPGElementSlot Slot, bool bFromPlayer)
{
	const int32 SlotIndex = static_cast<int32>(Slot);
	if (SlotIndex < 0 || SlotIndex >= UARPGMagicLoadout::SlotCount || !Loadout)
	{
		return;
	}

	if (!Loadout->IsSlotFilled(CurrentPage, Slot))
	{
		return;
	}

	const int32 Bit = 1 << SlotIndex;

	// Additive-only: already in the mix means nothing happens. Elements are
	// cleared wholesale or not at all -- see the class comment.
	if (ActiveMask & Bit)
	{
		return;
	}

	if (SelectionCooldown > 0.f)
	{
		return;
	}

	UARPGMagicElement* Element = Loadout->GetSlot(CurrentPage, Slot);

	if (UARPGMagicLoadout::ActiveCount(ActiveMask) >= Loadout->MaxElements)
	{
		OnElementActivationFailed.Broadcast(Slot, Element);
		return;
	}

	// THE GATE BEFORE THE COST, which reverses the Godot ordering deliberately.
	//
	// That ordering charged for the attempt on the reasoning that finding out
	// mid-mix that a pair is beyond you is part of the risk of experimenting. It
	// reads as a bug rather than as risk: nothing happens, the element does not
	// appear in the mix, and the mana is gone -- so the only feedback the player
	// gets for a refused pairing is a resource they cannot see being spent. A
	// block should be information.
	//
	// Cost is still paid for everything that DOES ready, which is what the rule
	// was actually about. This only stops charging for the ones that do not.
	if (!PassesComplexityGate(Element))
	{
		OnElementGateBlocked.Broadcast(Slot, Element);
		return;
	}

	if (Element && Element->ActivationCost > 0.f && !TrySpendMana(Element->ActivationCost))
	{
		OnElementActivationFailed.Broadcast(Slot, Element);
		return;
	}

	if (bFromPlayer)
	{
		CancelAutoReady();
	}

	ActiveMask |= Bit;
	SelectionCooldown = SelectionInterval;
	InvalidateCombinationCache();
	SetComponentTickEnabled(true);

	OnSelectionChanged.Broadcast(ActiveMask);

	if (UARPGMagicElement* Combination = GetResolvedCombination())
	{
		OnCombinationResolved.Broadcast(Combination);
	}
}

void UARPGMagicComponent::ClearSelection()
{
	if (GetOwner() && !GetOwner()->HasAuthority())
	{
		ServerClearSelection();
		return;
	}

	CancelAutoReady();

	// Silent when already empty, so observers do not see a change that did not
	// happen.
	if (ActiveMask == 0)
	{
		return;
	}

	ActiveMask = 0;
	SelectionCooldown = 0.f;
	InvalidateCombinationCache();
	OnSelectionChanged.Broadcast(ActiveMask);
}

void UARPGMagicComponent::ServerClearSelection_Implementation()
{
	ClearSelection();
}

void UARPGMagicComponent::SetCurrentPage(int32 Page)
{
	if (GetOwner() && !GetOwner()->HasAuthority())
	{
		ServerSetCurrentPage(Page);
		return;
	}

	if (!Loadout || Page < 0 || Page >= Loadout->PageCount || Page == CurrentPage)
	{
		return;
	}

	CurrentPage = Page;
	InvalidateCombinationCache();

	// The mask indexes slots on the CURRENT page, so carrying it across would
	// silently re-point every readied element at a different element.
	ClearSelection();
}

void UARPGMagicComponent::ServerSetCurrentPage_Implementation(int32 Page)
{
	SetCurrentPage(Page);
}

void UARPGMagicComponent::OnRep_ActiveMask()
{
	// The mask arrived from the server, so anything derived from it is stale.
	InvalidateCombinationCache();

	OnSelectionChanged.Broadcast(ActiveMask);

	if (UARPGMagicElement* Combination = GetResolvedCombination())
	{
		OnCombinationResolved.Broadcast(Combination);
	}
}

// ---------------------------------------------------------------------------
// Consumption
// ---------------------------------------------------------------------------

UARPGMagicElement* UARPGMagicComponent::ConsumeForImbue()
{
	UARPGMagicElement* Element = GetDisplayElement();
	if (!Element || !CanDischarge())
	{
		return nullptr;
	}

	// Flat: imbue is not chargeable, so there is no fraction to scale by.
	if (!TrySpendMana(GetUsageRate() * ImbueManaCost))
	{
		OnSpellFailed.Broadcast(GetActiveElementTags());
		return nullptr;
	}

	CancelAutoReady();
	LastDischargedMask = ActiveMask;
	ClearSelection();
	return Element;
}

UARPGMagicElement* UARPGMagicComponent::ConsumeForDodge()
{
	UARPGMagicElement* Element = GetDisplayElement();
	if (!Element || !CanDischarge())
	{
		return nullptr;
	}

	if (!TrySpendMana(GetUsageRate() * DodgeManaCost))
	{
		OnSpellFailed.Broadcast(GetActiveElementTags());
		return nullptr;
	}

	CancelAutoReady();
	LastDischargedMask = ActiveMask;
	ClearSelection();
	return Element;
}

float UARPGMagicComponent::GetImbueDamage(const UARPGMagicElement* Element, float MotionValue) const
{
	if (!Element)
	{
		return 0.f;
	}

	return Element->BaseDamage * ImbueDamageMultiplier
		* GetMasteryDamageMultiplier(Element) * MotionValue;
}

float UARPGMagicComponent::GetImbuePoiseDamage(const UARPGMagicElement* Element, float MotionValue) const
{
	if (!Element)
	{
		return 0.f;
	}

	// No mastery term, on purpose: poise stays independent of HP-damage power
	// creep, but still respects the static imbue multiplier.
	return Element->BasePoiseDamage * ImbueDamageMultiplier * MotionValue;
}

FARPGElementalCoating UARPGMagicComponent::BuildImbueCoating(const UARPGMagicElement* Element,
	float MotionValue) const
{
	FARPGElementalCoating Coating;
	if (!Element)
	{
		return Coating;
	}

	Coating.BaseDamage = GetImbueDamage(Element, MotionValue);
	Coating.PoiseDamage = GetImbuePoiseDamage(Element, MotionValue);
	Coating.DamageType = Element->DamageType;
	Coating.MagicElementTag = Element->ElementTag;
	Coating.TraceRadiusScale = Element->ImbueReachScale;

	// Read off whichever element is active at the moment of consumption, so a
	// resolved combination brings its own status, damage type and reach rather
	// than either half's.
	if (Element->OnHitEffect)
	{
		Coating.OnHitEffects.Add(Element->OnHitEffect);
		Coating.OnHitEffectDuration = Element->StatusDuration;
	}

	return Coating;
}

// ---------------------------------------------------------------------------
// Discharge
// ---------------------------------------------------------------------------

FARPGDischargeTypeSettings UARPGMagicComponent::GetDischargeSettings(EARPGDischargeType Type) const
{
	if (const FARPGDischargeTypeSettings* Found = DischargeSettings.Find(Type))
	{
		return *Found;
	}
	return FARPGDischargeTypeSettings();
}

float UARPGMagicComponent::GetUsageRate() const
{
	// A combination REPLACES the sum rather than adding to it, so a combo spell
	// is priced on its own terms instead of inheriting its ingredients' costs.
	if (const UARPGMagicElement* Combination = GetResolvedCombination())
	{
		return Combination->UsageRate;
	}

	float Total = 0.f;
	for (const UARPGMagicElement* Element : GetActiveElements())
	{
		Total += Element->UsageRate;
	}
	return Total;
}

float UARPGMagicComponent::GetDischargeManaCost(EARPGDischargeType Type, float Charge) const
{
	const FARPGDischargeTypeSettings Settings = GetDischargeSettings(Type);
	return GetUsageRate() * FMath::Lerp(Settings.MinManaCost, Settings.MaxManaCost,
		FMath::Clamp(Charge, 0.f, 1.f));
}

FARPGDischargeContext UARPGMagicComponent::BuildDischargeContext(EARPGDischargeType Type,
	FVector Origin, FVector Direction, float Charge, bool bForcedRelease) const
{
	FARPGDischargeContext Context;
	Context.DischargeType = Type;
	Context.Caster = GetOwner();
	Context.Origin = Origin;
	Context.Direction = Direction;
	Context.Charge = FMath::Clamp(Charge, 0.f, 1.f);
	Context.bForcedRelease = bForcedRelease;
	Context.ActiveElements = GetActiveElementTags();

	UARPGMagicElement* Combination = GetResolvedCombination();
	UARPGMagicElement* Primary = Combination ? Combination : GetDisplayElement();
	Context.CombinationElement = Combination;
	Context.PrimaryElement = Primary;

	if (!Primary)
	{
		return Context;
	}

	const FARPGDischargeTypeSettings Settings = GetDischargeSettings(Type);
	const float ClampedCharge = Context.Charge;
	const float AmpMultiplier = GetDamageAmpMultiplier();

	// --- Power: cosmetic only. Never feeds damage. -------------------------
	// Proficiency is normalised against level 4, the point at which an element
	// is considered mastered.
	const float ProficiencyFraction = FMath::Clamp(GetEffectiveLevel(Primary) / 4.f, 0.f, 1.f);
	const float PowerBonus = ProficiencyFraction * MasteryPowerBonus;

	Context.MinPower = MinPower;
	Context.MaxPower = MaxPower;
	Context.PowerFraction = (FMath::Lerp(MinPower, MaxPower, ClampedCharge) + PowerBonus) * AmpMultiplier;

	// --- Damage: its own curve, sharing nothing with power above. ----------
	const float ChargeMultiplier = 1.f + ClampedCharge * Settings.MaxChargeDamage;

	Context.ComputedDamage = Primary->BaseDamage
		* ChargeMultiplier
		* GetMasteryDamageMultiplier(Primary)
		* Settings.DamageMultiplier
		* AmpMultiplier;

	// --- Poise: the two STATIC knobs only. ---------------------------------
	// Charge ratio and per-type multiplier are authored balance; mastery and
	// damage amp are dynamic runtime buffs, and poise deliberately does not
	// inherit those -- it is a separate axis from HP-damage power creep.
	Context.ComputedPoiseDamage = Primary->BasePoiseDamage
		* ChargeMultiplier
		* Settings.DamageMultiplier;

	return Context;
}

void UARPGMagicComponent::NotifyDischarged(const FARPGDischargeContext& Context)
{
	// Raised BEFORE the clear, so a listener still sees what was readied.
	OnDischargeExecuted.Broadcast(Context);

	// Remembered before the clear, so pressing cast again with nothing readied
	// re-readies exactly what was just cast.
	LastDischargedMask = ActiveMask;
	ClearSelection();
}

// ---------------------------------------------------------------------------
// Auto-ready
// ---------------------------------------------------------------------------

void UARPGMagicComponent::BeginAutoReady()
{
	if (LastDischargedMask == 0 || IsAutoReadyPending())
	{
		return;
	}

	AutoReadyMask = LastDischargedMask;
	AutoReadyTimer = SelectionInterval;
	SetComponentTickEnabled(true);
}

void UARPGMagicComponent::CancelAutoReady()
{
	AutoReadyMask = 0;
	AutoReadyTimer = -1.f;
}

void UARPGMagicComponent::AutoReadyStep()
{
	for (int32 SlotIndex = 0; SlotIndex < UARPGMagicLoadout::SlotCount; ++SlotIndex)
	{
		const int32 Bit = 1 << SlotIndex;
		if ((AutoReadyMask & Bit) == 0)
		{
			continue;
		}

		AutoReadyMask &= ~Bit;

		// Zero the rate limit first. Step timing is AutoReadyTimer's job, and
		// leaving the cooldown in place means two timers with the same period
		// racing each other -- which works only while they happen to stay in
		// lockstep, and silently drops a step the moment they do not.
		SelectionCooldown = 0.f;

		// Not bFromPlayer: this IS the auto-ready, and cancelling here would
		// stop the sequence on its own first step.
		ActivateSlot(static_cast<EARPGElementSlot>(SlotIndex), /*bFromPlayer=*/false);
		break;
	}

	AutoReadyTimer = AutoReadyMask != 0 ? SelectionInterval : -1.f;
}

// ---------------------------------------------------------------------------

void UARPGMagicComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (SelectionCooldown > 0.f)
	{
		SelectionCooldown = FMath::Max(0.f, SelectionCooldown - DeltaTime);
	}

	// The two timers below are the only per-frame work here, and both are idle
	// except in the moments right after a selection.
	if (SelectionCooldown <= 0.f && AutoReadyTimer < 0.f)
	{
		SetComponentTickEnabled(false);
	}

	// Server-only: each step spends mana and mutates the replicated mask.
	if (AutoReadyTimer >= 0.f && GetOwner() && GetOwner()->HasAuthority())
	{
		AutoReadyTimer -= DeltaTime;
		if (AutoReadyTimer <= 0.f)
		{
			AutoReadyStep();
		}
	}
}
