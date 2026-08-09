// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ARPGQuickSlotComponent.generated.h"

class UARPGInventoryComponent;
class UARPGItemDefinition;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnQuickSlotSelected, int32, SlotIndex);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnConsumableUsed, UARPGItemDefinition*, Item);

/**
 * The quick-access consumable bar. Port of Godot's QuickSlotComponent.
 *
 * SLOTS HOLD AN ITEM ID, NOT AN ITEM. A slot keeps pointing at "health_potion"
 * as stacks are drunk and picked back up; the live definition and quantity are
 * resolved from the inventory on demand. Holding the asset would mean the
 * binding either vanished when the last potion was drunk or held a stale
 * reference to a stack that no longer exists.
 *
 * A slot whose stock has run out STAYS assigned and stays selectable, so UI
 * greys it out rather than the bar silently reshuffling under the player's
 * thumb the moment they drink their last one.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCOMBAT_API UARPGQuickSlotComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGQuickSlotComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/** One id per slot; None for empty. Resizing keeps what fits. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|QuickSlots")
	TArray<FName> Slots;

	UFUNCTION(BlueprintCallable, Category = "ARPG|QuickSlots")
	void SetSlotCount(int32 Count);

	/**
	 * Assigning an id already on the bar clears the other slot, so the same
	 * consumable cannot occupy two slots and split the player's attention
	 * between bindings that do the same thing.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|QuickSlots")
	void AssignSlot(int32 Index, FName ItemId);

	UFUNCTION(BlueprintCallable, Category = "ARPG|QuickSlots")
	void ClearSlot(int32 Index);

	UFUNCTION(BlueprintPure, Category = "ARPG|QuickSlots")
	FName GetSlot(int32 Index) const;

	/** Index of the slot holding this id, or INDEX_NONE. */
	UFUNCTION(BlueprintPure, Category = "ARPG|QuickSlots")
	int32 FindSlot(FName ItemId) const;

	UFUNCTION(BlueprintPure, Category = "ARPG|QuickSlots")
	int32 FindFreeSlot() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|QuickSlots")
	int32 GetSelectedIndex() const { return SelectedIndex; }

	UFUNCTION(BlueprintCallable, Category = "ARPG|QuickSlots")
	void SetSelectedIndex(int32 Index);

	/**
	 * Steps to the next ASSIGNED slot, wrapping. Negative goes left.
	 *
	 * Skipping empty slots is what makes a D-pad usable: paging through four
	 * blanks to reach the potion is the behaviour of a bar that does not know
	 * what is on it.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|QuickSlots")
	void CycleSelection(int32 Direction);

	/** Where CycleSelection would land, without moving. Returns the current index when nothing else is assigned. */
	UFUNCTION(BlueprintPure, Category = "ARPG|QuickSlots")
	int32 PeekSelection(int32 Direction) const;

	UFUNCTION(BlueprintPure, Category = "ARPG|QuickSlots")
	UARPGItemDefinition* GetSlotItem(int32 Index) const;

	UFUNCTION(BlueprintPure, Category = "ARPG|QuickSlots")
	int32 GetSlotQuantity(int32 Index) const;

	UFUNCTION(BlueprintPure, Category = "ARPG|QuickSlots")
	UARPGItemDefinition* GetSelectedItem() const { return GetSlotItem(SelectedIndex); }

	UFUNCTION(BlueprintPure, Category = "ARPG|QuickSlots")
	int32 GetSelectedQuantity() const { return GetSlotQuantity(SelectedIndex); }

	/**
	 * Consumes one of the selected slot's item: applies its effects, decrements
	 * the inventory, and starts the cooldown.
	 *
	 * Returns false changing NOTHING when the slot is empty, the stock has run
	 * out, the item is not a consumable, or the cooldown is still running.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|QuickSlots")
	bool UseSelected();

	UFUNCTION(BlueprintPure, Category = "ARPG|QuickSlots")
	float GetCooldownRemaining() const { return CooldownRemaining; }

	UFUNCTION(BlueprintPure, Category = "ARPG|QuickSlots")
	bool IsReady() const { return CooldownRemaining <= 0.f; }

	UPROPERTY(BlueprintAssignable, Category = "ARPG|QuickSlots")
	FARPGOnQuickSlotSelected OnSelectionChanged;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|QuickSlots")
	FARPGOnConsumableUsed OnConsumableUsed;

private:
	UARPGInventoryComponent* GetInventory() const;

	bool IsValidIndex(int32 Index) const { return Slots.IsValidIndex(Index); }

	/**
	 * The last definition each slot's id resolved to.
	 *
	 * A display cache, deliberately not serialised: it is what lets a slot keep
	 * showing a potion's name and icon, greyed, after the last one is drunk,
	 * instead of the slot going blank.
	 */
	UPROPERTY(Transient)
	mutable TArray<TObjectPtr<UARPGItemDefinition>> CachedItems;

	UPROPERTY(Transient)
	mutable TObjectPtr<UARPGInventoryComponent> CachedInventory;

	int32 SelectedIndex = 0;
	float CooldownRemaining = 0.f;
};
