// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ARPGInventoryComponent.generated.h"

class UARPGItemDefinition;

/** One occupied slot. */
USTRUCT(BlueprintType)
struct ARPGCOMBAT_API FARPGInventorySlot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Inventory")
	TObjectPtr<UARPGItemDefinition> Item = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Inventory")
	int32 Quantity = 0;

	/**
	 * Unique for this slot's lifetime and never reused.
	 *
	 * Needed because "the item" is not a sufficient handle once the same item
	 * occupies several slots: dropping half a stack has to name WHICH stack, and
	 * an index would be invalidated by any removal before it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Inventory")
	int32 InstanceId = 0;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FARPGOnInventoryChanged,
	UARPGItemDefinition*, Item, int32, NewQuantity);

/**
 * A bag. Port of Godot's InventoryComponent.
 *
 * Items stack to their own MaxStackSize; adding beyond a full stack opens
 * another slot, up to Capacity. Capacity counts SLOTS, not units, so a hundred
 * potions in one stack cost one slot -- which is what makes stack size a
 * meaningful thing to author per item.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCOMBAT_API UARPGInventoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGInventoryComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Inventory",
		meta = (ClampMin = "1"))
	int32 Capacity = 20;

	UFUNCTION(BlueprintPure, Category = "ARPG|Inventory")
	int32 GetUsedSlots() const { return Slots.Num(); }

	UFUNCTION(BlueprintPure, Category = "ARPG|Inventory")
	bool IsFull() const { return Slots.Num() >= Capacity; }

	/**
	 * Fills existing stacks first, then opens new slots for the remainder.
	 *
	 * Returns HOW MANY were actually added, which may be fewer than asked for.
	 * A partial add is a real outcome, not a failure: refusing an entire pickup
	 * because its last two units do not fit reads as a bug, and a bool could not
	 * tell the caller how much to leave on the ground.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Inventory")
	int32 AddItem(UARPGItemDefinition* Item, int32 Quantity = 1);

	/** Returns false without removing anything when the bag holds too few. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Inventory")
	bool RemoveItem(FName ItemId, int32 Quantity = 1);

	/** Removes from ONE named slot, ignoring other stacks of the same item. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Inventory")
	bool RemoveFromSlot(int32 InstanceId, int32 Quantity = 1);

	UFUNCTION(BlueprintPure, Category = "ARPG|Inventory")
	bool HasItem(FName ItemId) const { return GetItemQuantity(ItemId) > 0; }

	/** Total across every stack. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Inventory")
	int32 GetItemQuantity(FName ItemId) const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Inventory")
	UARPGItemDefinition* FindItem(FName ItemId) const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Inventory")
	const TArray<FARPGInventorySlot>& GetSlots() const { return Slots; }

	UFUNCTION(BlueprintCallable, Category = "ARPG|Inventory")
	void Clear();

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Inventory")
	FARPGOnInventoryChanged OnItemAdded;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Inventory")
	FARPGOnInventoryChanged OnItemRemoved;

private:
	UPROPERTY()
	TArray<FARPGInventorySlot> Slots;

	/** Monotonic, never reused -- see FARPGInventorySlot::InstanceId. */
	int32 NextInstanceId = 1;
};
