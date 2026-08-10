// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGHudWidget.h"
#include "ARPGComboComponent.h"
#include "ARPGElementPalette.h"
#include "ARPGGameplayTags.h"
#include "ARPGItemDefinition.h"
#include "ARPGLocomotionComponent.h"
#include "ARPGMagicComponent.h"
#include "ARPGMagicElement.h"
#include "ARPGMagicLoadout.h"
#include "ARPGParryComponent.h"
#include "ARPGQuickSlotComponent.h"
#include "ARPGStatusEffectComponent.h"
#include "ARPGVitalSet.h"
#include "ARPGWeaponComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "GameFramework/PlayerController.h"

// ---------------------------------------------------------------------------
// Subject and component lookup
// ---------------------------------------------------------------------------

AActor* UARPGHudWidget::GetSubject() const
{
	const APlayerController* PC = GetOwningPlayer();
	return PC ? PC->GetPawn() : nullptr;
}

UAbilitySystemComponent* UARPGHudWidget::GetASC() const
{
	return UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetSubject());
}

UARPGMagicComponent* UARPGHudWidget::GetMagic() const
{
	AActor* Subject = GetSubject();
	return Subject ? Subject->FindComponentByClass<UARPGMagicComponent>() : nullptr;
}

UARPGComboComponent* UARPGHudWidget::GetCombo() const
{
	AActor* Subject = GetSubject();
	return Subject ? Subject->FindComponentByClass<UARPGComboComponent>() : nullptr;
}

UARPGWeaponComponent* UARPGHudWidget::GetWeapon() const
{
	AActor* Subject = GetSubject();
	return Subject ? Subject->FindComponentByClass<UARPGWeaponComponent>() : nullptr;
}

UARPGParryComponent* UARPGHudWidget::GetParry() const
{
	AActor* Subject = GetSubject();
	return Subject ? Subject->FindComponentByClass<UARPGParryComponent>() : nullptr;
}

UARPGQuickSlotComponent* UARPGHudWidget::GetQuickSlots() const
{
	AActor* Subject = GetSubject();
	return Subject ? Subject->FindComponentByClass<UARPGQuickSlotComponent>() : nullptr;
}

UARPGLocomotionComponent* UARPGHudWidget::GetLocomotion() const
{
	AActor* Subject = GetSubject();
	return Subject ? Subject->FindComponentByClass<UARPGLocomotionComponent>() : nullptr;
}

// ---------------------------------------------------------------------------
// Vitals
// ---------------------------------------------------------------------------

float UARPGHudWidget::GetAttribute(const FGameplayAttribute& Attribute) const
{
	const UAbilitySystemComponent* ASC = GetASC();
	return ASC ? ASC->GetNumericAttribute(Attribute) : 0.f;
}

float UARPGHudWidget::GetFraction(const FGameplayAttribute& Current,
	const FGameplayAttribute& Max) const
{
	const float Maximum = GetAttribute(Max);
	return Maximum > KINDA_SMALL_NUMBER
		? FMath::Clamp(GetAttribute(Current) / Maximum, 0.f, 1.f)
		: 0.f;
}

float UARPGHudWidget::GetHealth() const { return GetAttribute(UARPGVitalSet::GetHealthAttribute()); }
float UARPGHudWidget::GetMaxHealth() const { return GetAttribute(UARPGVitalSet::GetMaxHealthAttribute()); }
float UARPGHudWidget::GetMana() const { return GetAttribute(UARPGVitalSet::GetManaAttribute()); }
float UARPGHudWidget::GetMaxMana() const { return GetAttribute(UARPGVitalSet::GetMaxManaAttribute()); }
float UARPGHudWidget::GetStamina() const { return GetAttribute(UARPGVitalSet::GetStaminaAttribute()); }
float UARPGHudWidget::GetMaxStamina() const { return GetAttribute(UARPGVitalSet::GetMaxStaminaAttribute()); }

float UARPGHudWidget::GetHealthFraction() const
{
	return GetFraction(UARPGVitalSet::GetHealthAttribute(), UARPGVitalSet::GetMaxHealthAttribute());
}

float UARPGHudWidget::GetManaFraction() const
{
	return GetFraction(UARPGVitalSet::GetManaAttribute(), UARPGVitalSet::GetMaxManaAttribute());
}

float UARPGHudWidget::GetStaminaFraction() const
{
	return GetFraction(UARPGVitalSet::GetStaminaAttribute(), UARPGVitalSet::GetMaxStaminaAttribute());
}

float UARPGHudWidget::GetPoiseFraction() const
{
	// FILLS. A full poise bar is a stance about to break, not a healthy one --
	// so a HUD that draws it like health is telling the player the opposite of
	// what is happening. See UARPGPoiseComponent.
	return GetFraction(UARPGVitalSet::GetPoiseAttribute(), UARPGVitalSet::GetMaxPoiseAttribute());
}

bool UARPGHudWidget::IsDead() const
{
	const UAbilitySystemComponent* ASC = GetASC();
	return ASC && ASC->HasMatchingGameplayTag(TAG_State_Dead);
}

// ---------------------------------------------------------------------------
// Magic
// ---------------------------------------------------------------------------

TArray<FARPGHudElementSlot> UARPGHudWidget::GetElementSlots() const
{
	TArray<FARPGHudElementSlot> Result;

	const UARPGMagicComponent* Magic = GetMagic();
	const UARPGMagicLoadout* Loadout = Magic ? Magic->Loadout : nullptr;
	if (!Loadout)
	{
		return Result;
	}

	const float AvailableMana = Magic->GetAvailableMana();

	for (int32 Index = 0; Index < UARPGMagicLoadout::SlotCount; ++Index)
	{
		// Not "Slot": UUserWidget already has a member of that name for its own
		// parent panel slot, and shadowing it is an error at this warning level.
		const EARPGElementSlot ElementSlot = static_cast<EARPGElementSlot>(Index);

		FARPGHudElementSlot& Entry = Result.AddDefaulted_GetRef();

		const UARPGMagicElement* Element = Loadout->GetSlot(Magic->GetCurrentPage(), ElementSlot);
		if (!Element)
		{
			// An empty slot is still returned, so the HUD can draw four slots
			// unconditionally rather than laying out a variable number of them.
			continue;
		}

		Entry.ElementTag = Element->ElementTag;
		Entry.DisplayName = Element->DisplayName;
		Entry.ActivationCost = Element->ActivationCost;
		Entry.bReadied = Magic->IsSlotActive(ElementSlot);

		// Already-readied counts as affordable: the cost is spent, and drawing
		// it dimmed once the player is low would make what they are holding look
		// like it was about to be taken away.
		Entry.bAffordable = Entry.bReadied || AvailableMana >= Element->ActivationCost;

		Entry.Colour = Element->Palette ? Element->Palette->Glow : FLinearColor::White;
	}

	return Result;
}

int32 UARPGHudWidget::GetLoadoutPage() const
{
	const UARPGMagicComponent* Magic = GetMagic();
	return Magic ? Magic->GetCurrentPage() : 0;
}

int32 UARPGHudWidget::GetLoadoutPageCount() const
{
	const UARPGMagicComponent* Magic = GetMagic();
	const UARPGMagicLoadout* Loadout = Magic ? Magic->Loadout : nullptr;
	return Loadout ? Loadout->PageCount : 0;
}

FText UARPGHudWidget::GetReadiedElementName() const
{
	const UARPGMagicComponent* Magic = GetMagic();
	const UARPGMagicElement* Element = Magic ? Magic->GetDisplayElement() : nullptr;
	return Element ? Element->DisplayName : FText::GetEmpty();
}

FLinearColor UARPGHudWidget::GetReadiedElementColour() const
{
	const UARPGMagicComponent* Magic = GetMagic();
	const UARPGMagicElement* Element = Magic ? Magic->GetDisplayElement() : nullptr;
	return (Element && Element->Palette) ? Element->Palette->Glow : FLinearColor::White;
}

bool UARPGHudWidget::HasReadiedElements() const
{
	const UARPGMagicComponent* Magic = GetMagic();
	return Magic && Magic->HasActiveElements();
}

bool UARPGHudWidget::IsReadiedMixUncastable() const
{
	const UARPGMagicComponent* Magic = GetMagic();
	return Magic && Magic->HasActiveElements() && !Magic->CanDischarge();
}

// ---------------------------------------------------------------------------
// Combat
// ---------------------------------------------------------------------------

bool UARPGHudWidget::IsWeaponDrawn() const
{
	const UARPGWeaponComponent* Weapon = GetWeapon();
	return Weapon && Weapon->IsDrawn();
}

bool UARPGHudWidget::IsBlocking() const
{
	const UARPGParryComponent* Parry = GetParry();
	return Parry && Parry->IsBlocking();
}

bool UARPGHudWidget::IsParryActive() const
{
	const UARPGParryComponent* Parry = GetParry();
	return Parry && Parry->IsParryActive();
}

bool UARPGHudWidget::IsAttacking() const
{
	const UARPGComboComponent* Combo = GetCombo();
	return Combo && Combo->IsAttacking();
}

float UARPGHudWidget::GetAttackChargeFraction() const
{
	const UARPGComboComponent* Combo = GetCombo();

	// Only while actually charging. The combo component LATCHES the fraction at
	// release so the swing it scales can read it, which means reading it
	// unconditionally would leave a full charge meter on screen through the
	// entire attack that spent it.
	return (Combo && Combo->IsCharging()) ? Combo->GetChargeFraction() : 0.f;
}

bool UARPGHudWidget::IsSprinting() const
{
	const UARPGLocomotionComponent* Locomotion = GetLocomotion();
	return Locomotion && Locomotion->IsSprinting();
}

TArray<FARPGHudStatus> UARPGHudWidget::GetStatuses() const
{
	TArray<FARPGHudStatus> Result;

	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC)
	{
		return Result;
	}

	const TArray<FActiveGameplayEffectHandle> Handles = ASC->GetActiveEffects(FGameplayEffectQuery());
	const float WorldTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	for (int32 Index = 0; Index < Handles.Num(); ++Index)
	{
		const UGameplayEffect* Effect = ASC->GetGameplayEffectDefForHandle(Handles[Index]);
		const UARPGStatusEffectComponent* Presentation =
			Effect ? Effect->FindComponent<UARPGStatusEffectComponent>() : nullptr;

		// Only effects that describe themselves as a status. Everything else on
		// an ability system -- costs, cooldowns, an attack's own bookkeeping --
		// is machinery, and listing it would bury the four things that matter.
		if (!Presentation || !Presentation->StatusTag.IsValid())
		{
			continue;
		}

		FARPGHudStatus& Entry = Result.AddDefaulted_GetRef();
		Entry.StatusTag = Presentation->StatusTag;
		Entry.DisplayName = Presentation->DisplayName;
		Entry.bIsDebuff = Presentation->bIsDebuff;
		Entry.Stacks = FMath::Max(1, ASC->GetCurrentStackCount(Handles[Index]));

		// Asked of the ACTIVE EFFECT rather than taken from a parallel
		// GetActiveEffectsTimeRemaining array. Those two lists are built from
		// separate queries, and pairing them by index assumes an ordering
		// neither promises -- a status showing another status's timer is the
		// kind of bug that only appears once several are running at once.
		const FActiveGameplayEffect* Active = ASC->GetActiveGameplayEffect(Handles[Index]);
		Entry.TimeRemaining = Active ? Active->GetTimeRemaining(WorldTime) : -1.f;

		// Synchronous, and only for something already on screen: a status icon
		// that faded in a frame late would be worse than the tiny hitch.
		Entry.Icon = Presentation->Icon.LoadSynchronous();
	}

	return Result;
}

// ---------------------------------------------------------------------------
// Quick slots
// ---------------------------------------------------------------------------

FText UARPGHudWidget::GetSelectedConsumableName() const
{
	const UARPGQuickSlotComponent* QuickSlots = GetQuickSlots();
	const UARPGItemDefinition* Item = QuickSlots ? QuickSlots->GetSelectedItem() : nullptr;
	return Item ? Item->DisplayName : FText::GetEmpty();
}

int32 UARPGHudWidget::GetSelectedConsumableQuantity() const
{
	const UARPGQuickSlotComponent* QuickSlots = GetQuickSlots();
	return QuickSlots ? QuickSlots->GetSelectedQuantity() : 0;
}

int32 UARPGHudWidget::GetSelectedQuickSlotIndex() const
{
	const UARPGQuickSlotComponent* QuickSlots = GetQuickSlots();
	return QuickSlots ? QuickSlots->GetSelectedIndex() : 0;
}

bool UARPGHudWidget::IsQuickSlotReady() const
{
	const UARPGQuickSlotComponent* QuickSlots = GetQuickSlots();
	return QuickSlots && QuickSlots->IsReady();
}

// ---------------------------------------------------------------------------
// Bound widgets
// ---------------------------------------------------------------------------

void UARPGHudWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	RefreshBoundWidgets();
}

FLinearColor UARPGHudWidget::ResolveSlotTint(const FARPGHudElementSlot& Slot)
{
	if (!Slot.ElementTag.IsValid())
	{
		// An empty slot is drawn as a faint outline rather than hidden, so the
		// four positions stay stable and the player learns where things live.
		return FLinearColor(1.f, 1.f, 1.f, 0.12f);
	}

	FLinearColor Tint = Slot.Colour;

	if (Slot.bReadied)
	{
		Tint.A = 1.f;
	}
	else if (!Slot.bAffordable)
	{
		Tint.A = 0.25f;
	}
	else
	{
		Tint.A = 0.65f;
	}

	return Tint;
}

void UARPGHudWidget::RefreshBoundWidgets()
{
	if (HealthBar)  { HealthBar->SetPercent(GetHealthFraction()); }
	if (ManaBar)    { ManaBar->SetPercent(GetManaFraction()); }
	if (StaminaBar) { StaminaBar->SetPercent(GetStaminaFraction()); }
	if (PoiseBar)   { PoiseBar->SetPercent(GetPoiseFraction()); }

	const TArray<FARPGHudElementSlot> Slots = GetElementSlots();
	UImage* const Swatches[] = {
		ElementSlotNorth, ElementSlotWest, ElementSlotSouth, ElementSlotEast
	};

	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Swatches); ++Index)
	{
		if (!Swatches[Index])
		{
			continue;
		}

		// An unconfigured loadout yields no slots at all; the swatches still get
		// their empty tint rather than keeping whatever they were last frame.
		Swatches[Index]->SetColorAndOpacity(Slots.IsValidIndex(Index)
			? ResolveSlotTint(Slots[Index])
			: ResolveSlotTint(FARPGHudElementSlot()));
	}

	if (ReadiedElementLabel)
	{
		// The uncastable case is called out in words. A failed mix and a working
		// one look identical from the player's side until the trigger comes up
		// and nothing happens, which reads as the spell being broken.
		const FText Name = GetReadiedElementName();
		ReadiedElementLabel->SetText(IsReadiedMixUncastable()
			? NSLOCTEXT("ARPGHud", "UncastableMix", "No such combination")
			: Name);
		ReadiedElementLabel->SetColorAndOpacity(FSlateColor(GetReadiedElementColour()));
	}

	if (PageLabel)
	{
		const int32 Count = GetLoadoutPageCount();
		PageLabel->SetText(Count > 0
			? FText::Format(NSLOCTEXT("ARPGHud", "PageFormat", "Page {0}/{1}"),
				FText::AsNumber(GetLoadoutPage() + 1), FText::AsNumber(Count))
			: FText::GetEmpty());
	}

	if (QuickSlotLabel)
	{
		const int32 Quantity = GetSelectedConsumableQuantity();
		const FText Name = GetSelectedConsumableName();

		QuickSlotLabel->SetText(Name.IsEmpty()
			? NSLOCTEXT("ARPGHud", "EmptyQuickSlot", "-")
			: FText::Format(NSLOCTEXT("ARPGHud", "QuickSlotFormat", "{0} x{1}"),
				Name, FText::AsNumber(Quantity)));

		// Dimmed while on cooldown or empty: the bar's job is to answer "can I
		// drink right now", and a count alone does not.
		const bool bUsable = Quantity > 0 && IsQuickSlotReady();
		QuickSlotLabel->SetOpacity(bUsable ? 1.f : 0.4f);
	}
}

// ---------------------------------------------------------------------------
// Default layout
// ---------------------------------------------------------------------------

namespace
{
	constexpr float BarWidth = 320.f;
	constexpr float BarHeight = 18.f;
	constexpr float BarGap = 6.f;
	constexpr float Margin = 40.f;
	constexpr float Swatch = 42.f;
	constexpr float SwatchGap = 6.f;

	/** Anchors as a single corner, which is how every element here is placed. */
	FAnchors CornerAnchors(float X, float Y)
	{
		return FAnchors(X, Y, X, Y);
	}

	UCanvasPanelSlot* PlaceAt(UCanvasPanel* Canvas, UWidget* Widget,
		const FAnchors& Anchors, const FVector2D& Position, const FVector2D& Size)
	{
		UCanvasPanelSlot* Slot = Canvas->AddChildToCanvas(Widget);
		if (!Slot)
		{
			return nullptr;
		}

		Slot->SetAnchors(Anchors);
		Slot->SetAutoSize(false);
		Slot->SetPosition(Position);
		Slot->SetSize(Size);
		return Slot;
	}

	void StyleBar(UProgressBar* Bar, const FLinearColor& Fill)
	{
		FProgressBarStyle Style = Bar->GetWidgetStyle();
		Style.BackgroundImage.TintColor = FSlateColor(FLinearColor(0.02f, 0.02f, 0.02f, 0.75f));
		Style.FillImage.TintColor = FSlateColor(Fill);
		Bar->SetWidgetStyle(Style);
		Bar->SetPercent(0.f);
	}
}

TSharedRef<SWidget> UARPGHudWidget::RebuildWidget()
{
	// A Blueprint subclass brings its own root. Building over it would replace
	// an authored HUD with this stand-in, which is the opposite of the point.
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildDefaultLayout();
	}

	return Super::RebuildWidget();
}

void UARPGHudWidget::BuildDefaultLayout()
{
	UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(
		UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
	WidgetTree->RootWidget = Canvas;

	const FAnchors TopLeft = CornerAnchors(0.f, 0.f);
	const FAnchors BottomLeft = CornerAnchors(0.f, 1.f);
	const FAnchors BottomRight = CornerAnchors(1.f, 1.f);

	// Vitals, top left, in the order they are spent -- and poise last, set apart,
	// because it is the only one that fills rather than drains.
	struct FBarSpec
	{
		const TCHAR* Name;
		TObjectPtr<UProgressBar>* Target;
		FLinearColor Fill;
	};

	const FBarSpec BarSpecs[] = {
		{ TEXT("HealthBar"),  &HealthBar,  FLinearColor(0.75f, 0.15f, 0.15f) },
		{ TEXT("ManaBar"),    &ManaBar,    FLinearColor(0.20f, 0.40f, 0.85f) },
		{ TEXT("StaminaBar"), &StaminaBar, FLinearColor(0.25f, 0.70f, 0.30f) },
		{ TEXT("PoiseBar"),   &PoiseBar,   FLinearColor(0.85f, 0.70f, 0.20f) },
	};

	for (int32 Index = 0; Index < UE_ARRAY_COUNT(BarSpecs); ++Index)
	{
		UProgressBar* Bar = WidgetTree->ConstructWidget<UProgressBar>(
			UProgressBar::StaticClass(), BarSpecs[Index].Name);

		StyleBar(Bar, BarSpecs[Index].Fill);
		PlaceAt(Canvas, Bar, TopLeft,
			FVector2D(Margin, Margin + Index * (BarHeight + BarGap)),
			FVector2D(BarWidth, BarHeight));

		*BarSpecs[Index].Target = Bar;
	}

	// Element slots, bottom left, arranged the way the face buttons are -- so the
	// HUD and the controller agree about which slot is which without the player
	// having to learn a mapping.
	struct FSwatchSpec
	{
		const TCHAR* Name;
		TObjectPtr<UImage>* Target;
		FVector2D Offset;
	};

	const FSwatchSpec SwatchSpecs[] = {
		{ TEXT("ElementSlotNorth"), &ElementSlotNorth, FVector2D(0.f, -Swatch - SwatchGap) },
		{ TEXT("ElementSlotWest"),  &ElementSlotWest,  FVector2D(-Swatch - SwatchGap, 0.f) },
		{ TEXT("ElementSlotEast"),  &ElementSlotEast,  FVector2D(Swatch + SwatchGap, 0.f) },
		{ TEXT("ElementSlotSouth"), &ElementSlotSouth, FVector2D(0.f, Swatch + SwatchGap) },
	};

	const FVector2D Centre(Margin + Swatch + SwatchGap, -(Margin + Swatch + SwatchGap));

	for (const FSwatchSpec& Spec : SwatchSpecs)
	{
		UImage* Image = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), Spec.Name);
		PlaceAt(Canvas, Image, BottomLeft, Centre + Spec.Offset, FVector2D(Swatch, Swatch));
		*Spec.Target = Image;
	}

	ReadiedElementLabel = WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(), TEXT("ReadiedElementLabel"));
	PlaceAt(Canvas, ReadiedElementLabel, BottomLeft,
		FVector2D(Margin, Centre.Y - Swatch - 34.f), FVector2D(260.f, 24.f));

	PageLabel = WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(), TEXT("PageLabel"));
	PlaceAt(Canvas, PageLabel, BottomLeft,
		FVector2D(Margin, -Margin - 24.f), FVector2D(160.f, 24.f));

	QuickSlotLabel = WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(), TEXT("QuickSlotLabel"));
	PlaceAt(Canvas, QuickSlotLabel, BottomRight,
		FVector2D(-240.f - Margin, -Margin - 24.f), FVector2D(240.f, 24.f));
}
