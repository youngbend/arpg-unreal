// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGInventoryComponent.h"
#include "ARPGCombat.h"
#include "ARPGItemDefinition.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

UARPGInventoryComponent::UARPGInventoryComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false); // server-owned; the owning client reads it via UI
}

int32 UARPGInventoryComponent::AddItem(UARPGItemDefinition* Item, int32 Quantity)
{
	if (!Item || Quantity <= 0)
	{
		return 0;
	}

	const int32 MaxStack = FMath::Max(1, Item->MaxStackSize);
	int32 Remaining = Quantity;

	// Existing stacks first, so a pickup tops up what the player already carries
	// rather than opening a second slot beside a half-full one.
	for (FARPGInventorySlot& Slot : Slots)
	{
		if (Remaining <= 0)
		{
			break;
		}

		if (Slot.Item != Item || Slot.Quantity >= MaxStack)
		{
			continue;
		}

		const int32 Space = MaxStack - Slot.Quantity;
		const int32 Moved = FMath::Min(Space, Remaining);
		Slot.Quantity += Moved;
		Remaining -= Moved;
	}

	while (Remaining > 0 && !IsFull())
	{
		FARPGInventorySlot NewSlot;
		NewSlot.Item = Item;
		NewSlot.Quantity = FMath::Min(MaxStack, Remaining);
		NewSlot.InstanceId = NextInstanceId++;

		Remaining -= NewSlot.Quantity;
		Slots.Add(NewSlot);
	}

	const int32 Added = Quantity - Remaining;
	if (Added > 0)
	{
		OnItemAdded.Broadcast(Item, GetItemQuantity(Item->ItemId));
	}

	if (Remaining > 0)
	{
		UE_LOG(LogARPGCombat, Log, TEXT("Inventory full: %d of '%s' could not be added."),
			Remaining, *Item->ItemId.ToString());
	}

	return Added;
}

bool UARPGInventoryComponent::RemoveItem(FName ItemId, int32 Quantity)
{
	if (Quantity <= 0 || GetItemQuantity(ItemId) < Quantity)
	{
		// All or nothing. A partial removal would leave the caller believing it
		// had spent something it had not -- the opposite of AddItem, where a
		// partial result is the honest answer.
		return false;
	}

	UARPGItemDefinition* Item = FindItem(ItemId);
	int32 Remaining = Quantity;

	// From the END, so the newest partial stack is spent before older full ones
	// and the bag tends toward fewer, fuller slots.
	for (int32 Index = Slots.Num() - 1; Index >= 0 && Remaining > 0; --Index)
	{
		FARPGInventorySlot& Slot = Slots[Index];
		if (!Slot.Item || Slot.Item->ItemId != ItemId)
		{
			continue;
		}

		const int32 Taken = FMath::Min(Slot.Quantity, Remaining);
		Slot.Quantity -= Taken;
		Remaining -= Taken;

		if (Slot.Quantity <= 0)
		{
			Slots.RemoveAt(Index);
		}
	}

	OnItemRemoved.Broadcast(Item, GetItemQuantity(ItemId));
	return true;
}

bool UARPGInventoryComponent::RemoveFromSlot(int32 InstanceId, int32 Quantity)
{
	if (Quantity <= 0)
	{
		return false;
	}

	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		FARPGInventorySlot& Slot = Slots[Index];
		if (Slot.InstanceId != InstanceId)
		{
			continue;
		}

		if (Slot.Quantity < Quantity)
		{
			return false;
		}

		UARPGItemDefinition* Item = Slot.Item;
		Slot.Quantity -= Quantity;

		if (Slot.Quantity <= 0)
		{
			Slots.RemoveAt(Index);
		}

		OnItemRemoved.Broadcast(Item, Item ? GetItemQuantity(Item->ItemId) : 0);
		return true;
	}

	return false;
}

int32 UARPGInventoryComponent::GetItemQuantity(FName ItemId) const
{
	int32 Total = 0;
	for (const FARPGInventorySlot& Slot : Slots)
	{
		if (Slot.Item && Slot.Item->ItemId == ItemId)
		{
			Total += Slot.Quantity;
		}
	}
	return Total;
}

UARPGItemDefinition* UARPGInventoryComponent::FindItem(FName ItemId) const
{
	for (const FARPGInventorySlot& Slot : Slots)
	{
		if (Slot.Item && Slot.Item->ItemId == ItemId)
		{
			return Slot.Item;
		}
	}
	return nullptr;
}

void UARPGInventoryComponent::Clear()
{
	Slots.Reset();

	// Instance ids deliberately keep counting. Reusing them after a clear would
	// let a stale handle -- a dragged UI element, a queued action -- resolve to
	// an unrelated slot.
	OnItemRemoved.Broadcast(nullptr, 0);
}

#if WITH_EDITOR
EDataValidationResult UARPGItemDefinition::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	// Everything keys off the id: inventory lookups, quick-slot bindings, save
	// data. An unnamed item can be picked up and never found again.
	if (ItemId.IsNone())
	{
		Context.AddError(FText::FromString(
			TEXT("ItemId is unset. Inventory lookups and quick-slot bindings both key off it.")));
		Result = EDataValidationResult::Invalid;
	}

	const bool bHasPayload =
		(ItemType == EARPGItemType::Weapon && WeaponDefinition != nullptr)
		|| (ItemType == EARPGItemType::Armor && ArmorDefinition != nullptr)
		|| (ItemType == EARPGItemType::Consumable && ConsumableDefinition != nullptr);

	const bool bNeedsPayload =
		ItemType == EARPGItemType::Weapon
		|| ItemType == EARPGItemType::Armor
		|| ItemType == EARPGItemType::Consumable;

	if (bNeedsPayload && !bHasPayload)
	{
		Context.AddError(FText::FromString(
			TEXT("This item type needs its matching payload set, or it can be carried but never "
			     "equipped or used.")));
		Result = EDataValidationResult::Invalid;
	}

	if (IsEquipment() && MaxStackSize > 1)
	{
		Context.AddWarning(FText::FromString(
			TEXT("Equipment with a stack size above 1 will merge distinct pieces into one slot, "
			     "losing their individual modifiers.")));
	}

	return Result;
}
#endif
