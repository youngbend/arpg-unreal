// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CommonUserWidget.h"
#include "GameplayTagContainer.h"
#include "ARPGHudWidget.generated.h"

class AActor;
class UARPGComboComponent;
class UARPGItemDefinition;
class UARPGMagicElement;
class UARPGLocomotionComponent;
class UARPGMagicComponent;
class UARPGParryComponent;
class UARPGQuickSlotComponent;
class UARPGWeaponComponent;
class UAbilitySystemComponent;
class UImage;
class UProgressBar;
class UTextBlock;
class UTexture2D;

/** One element slot on the current loadout page, as the HUD needs to draw it. */
USTRUCT(BlueprintType)
struct FARPGHudElementSlot
{
	GENERATED_BODY()

	/** Empty when the page has nothing in this slot. */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|HUD")
	FGameplayTag ElementTag;

	UPROPERTY(BlueprintReadOnly, Category = "ARPG|HUD")
	FText DisplayName;

	/**
	 * The element's own glow, so the HUD needs no colour table of its own.
	 *
	 * White where an element has no palette -- which is also the case where it
	 * has no placeholder visual, so the slot and the world agree about an
	 * element being undressed.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|HUD")
	FLinearColor Colour = FLinearColor::White;

	UPROPERTY(BlueprintReadOnly, Category = "ARPG|HUD")
	bool bReadied = false;

	/** False when the mana to ready it is not there; draw it dimmed. */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|HUD")
	bool bAffordable = true;

	UPROPERTY(BlueprintReadOnly, Category = "ARPG|HUD")
	float ActivationCost = 0.f;
};

/** One status effect currently on the player. */
USTRUCT(BlueprintType)
struct FARPGHudStatus
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ARPG|HUD")
	FGameplayTag StatusTag;

	UPROPERTY(BlueprintReadOnly, Category = "ARPG|HUD")
	FText DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "ARPG|HUD")
	TObjectPtr<UTexture2D> Icon;

	UPROPERTY(BlueprintReadOnly, Category = "ARPG|HUD")
	int32 Stacks = 1;

	/** Seconds left, or -1 for something that does not expire on its own. */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|HUD")
	float TimeRemaining = -1.f;

	/** Debuffs and buffs usually want opposite treatment. */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|HUD")
	bool bIsDebuff = true;
};

/**
 * Everything the HUD draws, resolved from the player's own components.
 *
 * THE WIDGET ASSET IS PRESENTATION ONLY. All of the reading -- which attribute,
 * which component, what counts as "readied", how a page maps to four slots --
 * lives here in C++, so the Blueprint side is bindings and layout. That split is
 * what keeps the HUD from quietly becoming a second implementation of the rules:
 * a widget graph that decided for itself when an element was affordable would
 * disagree with the magic component the first time either changed.
 *
 * Every getter is safe with nothing wired up. A pawn with no magic component
 * reports no elements rather than failing, which is what lets the same HUD sit
 * over a character mid-build.
 *
 * Poise is the one number that reads backwards from the rest: it FILLS as hits
 * land and the stance breaks at full, so a poise bar is a threat meter, not a
 * health bar. See UARPGPoiseComponent.
 *
 * NOT ABSTRACT, AND IT BUILDS ITS OWN LAYOUT. Constructing a widget tree in C++
 * is unusual, and the reason here is specific: there is no supported way to
 * generate a widget Blueprint's tree from a script -- UBaseWidgetBlueprint's
 * WidgetTree is a bare UPROPERTY and is invisible to Python -- so the choice was
 * between a HUD that works out of the box and one that waits for somebody to
 * author it by hand.
 *
 * The layout below is deliberately plain: flat bars, plain text, no art. It is
 * meant to be READ while testing and then replaced, and something that looked
 * designed would read as a decision rather than a stand-in.
 *
 * TO RESTYLE IT, make a widget Blueprint deriving from this class. A subclass
 * with its own root supplies its own tree, so RebuildWidget leaves it alone
 * entirely and the BindWidgetOptional slots above pick your widgets up by name.
 * Nothing needs un-wiring first.
 */
UCLASS()
class UARPGHudWidget : public UCommonUserWidget
{
	GENERATED_BODY()

public:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/**
	 * Builds the default layout, ONLY when nothing else has.
	 *
	 * A Blueprint subclass arrives here with its tree already rooted, which is
	 * the check that keeps this from fighting an authored HUD.
	 */
	virtual TSharedRef<SWidget> RebuildWidget() override;

	// --- Bound widgets --------------------------------------------------------
	//
	// OPTIONAL bindings, matched by name. This is what lets the widget asset be
	// pure layout: name a progress bar HealthBar and it fills itself, delete it
	// and nothing breaks. The alternative -- property bindings in the widget
	// graph -- would put a copy of every rule back in Blueprint, which is what
	// the C++ getters above exist to avoid.
	//
	// Everything here is refreshed WHEN SOMETHING CHANGES, not every frame.
	//
	// It used to be driven unconditionally from NativeTick, on the argument that
	// a dozen reads a frame is cheaper than delegate plumbing and cannot go
	// stale. The first half was true; the second was the problem. Pulling meant
	// the HUD had to know which six components to interrogate and reach into all
	// of them, which is the coupling -- not the arithmetic. The components
	// already declared every delegate needed to be told instead, and nothing was
	// listening to any of them.
	//
	// So: bind once per possession, mark dirty on change, refresh at most once a
	// frame and only when there is something to say. See MarkDirty().

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UProgressBar> HealthBar;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UProgressBar> ManaBar;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UProgressBar> StaminaBar;

	/** Fills towards a stance break. See GetPoiseFraction. */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UProgressBar> PoiseBar;

	/** Four swatches, tinted from each element's own palette. */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UImage> ElementSlotNorth;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UImage> ElementSlotWest;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UImage> ElementSlotSouth;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UImage> ElementSlotEast;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> ReadiedElementLabel;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> PageLabel;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> QuickSlotLabel;

	// --- Vitals ---------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Vitals")
	float GetHealth() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Vitals")
	float GetMaxHealth() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Vitals")
	float GetHealthFraction() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Vitals")
	float GetMana() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Vitals")
	float GetMaxMana() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Vitals")
	float GetManaFraction() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Vitals")
	float GetStamina() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Vitals")
	float GetMaxStamina() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Vitals")
	float GetStaminaFraction() const;

	/** 0 is composed, 1 is about to break. Fills; it is not a health bar. */
	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Vitals")
	float GetPoiseFraction() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Vitals")
	bool IsDead() const;

	// --- Magic ----------------------------------------------------------------

	/** The four slots of the current page, in North/West/South/East order. */
	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Magic")
	TArray<FARPGHudElementSlot> GetElementSlots() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Magic")
	int32 GetLoadoutPage() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Magic")
	int32 GetLoadoutPageCount() const;

	/** The combination where one resolved, otherwise the single readied element. */
	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Magic")
	FText GetReadiedElementName() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Magic")
	FLinearColor GetReadiedElementColour() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Magic")
	bool HasReadiedElements() const;

	/**
	 * True when something is readied that cannot actually be cast -- two or more
	 * elements with no matching recipe.
	 *
	 * Worth showing, because from the player's side a failed mix and a working
	 * one look identical until the trigger comes up and nothing happens.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Magic")
	bool IsReadiedMixUncastable() const;

	// --- Combat ---------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Combat")
	bool IsWeaponDrawn() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Combat")
	bool IsBlocking() const;

	/** Guard is up AND inside the perfect-parry window. */
	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Combat")
	bool IsParryActive() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Combat")
	bool IsAttacking() const;

	/** 0-1 while winding up a charged attack, 0 otherwise. */
	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Combat")
	float GetAttackChargeFraction() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Combat")
	bool IsSprinting() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|Combat")
	TArray<FARPGHudStatus> GetStatuses() const;

	// --- Quick slots ----------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|QuickSlots")
	FText GetSelectedConsumableName() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|QuickSlots")
	int32 GetSelectedConsumableQuantity() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|QuickSlots")
	int32 GetSelectedQuickSlotIndex() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|HUD|QuickSlots")
	bool IsQuickSlotReady() const;

protected:
	/**
	 * The pawn this HUD is about.
	 *
	 * The owning player's CURRENT pawn. Re-resolved whenever possession changes
	 * -- a HUD holding the corpse is a HUD that stops updating without ever
	 * looking broken -- but CACHED between changes, along with every component
	 * hanging off it.
	 *
	 * Every getter below is a Blueprint property binding, which UMG evaluates
	 * once per bound widget per frame. Each one used to call GetPawn() and then
	 * FindComponentByClass, so a HUD with a dozen bindings walked the pawn's
	 * component array a dozen times a frame to reach components that had not
	 * moved.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|HUD")
	AActor* GetSubject() const;

	UAbilitySystemComponent* GetASC() const;
	UARPGMagicComponent* GetMagic() const;
	UARPGComboComponent* GetCombo() const;
	UARPGWeaponComponent* GetWeapon() const;
	UARPGParryComponent* GetParry() const;
	UARPGQuickSlotComponent* GetQuickSlots() const;
	UARPGLocomotionComponent* GetLocomotion() const;

private:
	/** Re-resolves the cache when the owning player's pawn has changed. */
	void RefreshSubject() const;

	UPROPERTY(Transient)
	mutable TWeakObjectPtr<AActor> CachedSubject;

	UPROPERTY(Transient)
	mutable TObjectPtr<UAbilitySystemComponent> CachedASC;

	UPROPERTY(Transient)
	mutable TObjectPtr<UARPGMagicComponent> CachedMagic;

	UPROPERTY(Transient)
	mutable TObjectPtr<UARPGComboComponent> CachedCombo;

	UPROPERTY(Transient)
	mutable TObjectPtr<UARPGWeaponComponent> CachedWeapon;

	UPROPERTY(Transient)
	mutable TObjectPtr<UARPGParryComponent> CachedParry;

	UPROPERTY(Transient)
	mutable TObjectPtr<UARPGQuickSlotComponent> CachedQuickSlots;

	UPROPERTY(Transient)
	mutable TObjectPtr<UARPGLocomotionComponent> CachedLocomotion;

protected:

	/** Attribute read that returns 0 rather than asserting with no ASC. */
	float GetAttribute(const struct FGameplayAttribute& Attribute) const;

	/** Current over max, guarded against a zero maximum. */
	float GetFraction(const struct FGameplayAttribute& Current,
		const struct FGameplayAttribute& Max) const;

	/** Pushes the current state into whichever bound widgets exist. */
	void RefreshBoundWidgets();

	/**
	 * Subscribes to everything that can change what this HUD shows.
	 *
	 * Called on every possession change, after the component pointers are
	 * resolved. Unbinding first is not optional -- a widget that survives two
	 * possessions would otherwise hold two subscriptions to the second pawn and
	 * one to a dead one.
	 */
	void BindSubjectDelegates();
	void UnbindSubjectDelegates();

	/** Queues a refresh for the next frame. Cheap enough to call from anything. */
	void MarkDirty() { bDirty = true; }

	// Delegate sinks. All of them do the same thing -- the HUD does not care
	// WHAT changed, only that something did -- but UFUNCTION-bound dynamic
	// delegates must match their signatures exactly, so each shape needs one.
	//
	// ONLY THE THREE SOURCES RefreshBoundWidgets ACTUALLY READS are bound:
	// attributes, the magic component and the quick slots. The combo, parry,
	// weapon and locomotion getters below exist for Blueprint to call on demand,
	// and a Blueprint asking a question when it wants the answer is already
	// pull-shaped -- subscribing on their behalf would be plumbing for nobody.
	UFUNCTION() void HandleSelectionChanged(int32 ActiveMask);
	UFUNCTION() void HandleCombinationResolved(UARPGMagicElement* Combination);
	UFUNCTION() void HandleQuickSlotSelected(int32 SlotIndex);
	UFUNCTION() void HandleConsumableUsed(UARPGItemDefinition* Item);

	/** Attribute-change sink. Not a UFUNCTION: GAS uses a plain delegate here. */
	void HandleAttributeChanged(const struct FOnAttributeChangeData& Data);

	/** Set by every change; cleared by the refresh it causes. */
	bool bDirty = true;

	/**
	 * True while the quick-slot cooldown is running.
	 *
	 * The one piece of state with no event behind it -- a cooldown just elapses.
	 * Rather than tick for everything on its account, the HUD stays dirty only
	 * while a cooldown is actually counting down, which is a second or two after
	 * a drink rather than every frame of the game.
	 */
	bool IsAwaitingCooldown() const;

	/** Attribute delegate handles, so the bindings can be undone on repossession. */
	TArray<FDelegateHandle> AttributeHandles;

	/** Constructs the plain default layout into an empty widget tree. */
	void BuildDefaultLayout();

	/**
	 * How a slot swatch reads when it is empty, unaffordable, or readied.
	 *
	 * Readied is drawn at full strength and unaffordable at a quarter, so the
	 * page answers "what can I do right now" at a glance rather than needing the
	 * player to remember their own mana.
	 */
	static FLinearColor ResolveSlotTint(const FARPGHudElementSlot& Slot);
};
