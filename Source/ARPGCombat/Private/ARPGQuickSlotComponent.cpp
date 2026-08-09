// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGQuickSlotComponent.h"
#include "ARPGCombat.h"
#include "ARPGConsumableDefinition.h"
#include "ARPGInventoryComponent.h"
#include "ARPGItemDefinition.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "GameplayEffect.h"

UARPGQuickSlotComponent::UARPGQuickSlotComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	Slots.SetNum(4);
}

UARPGInventoryComponent* UARPGQuickSlotComponent::GetInventory() const
{
	if (!CachedInventory && GetOwner())
	{
		CachedInventory = GetOwner()->FindComponentByClass<UARPGInventoryComponent>();
	}
	return CachedInventory;
}

void UARPGQuickSlotComponent::SetSlotCount(int32 Count)
{
	Slots.SetNum(FMath::Max(0, Count));
	CachedItems.SetNum(Slots.Num());
	SetSelectedIndex(FMath::Clamp(SelectedIndex, 0, FMath::Max(0, Slots.Num() - 1)));
}

void UARPGQuickSlotComponent::AssignSlot(int32 Index, FName ItemId)
{
	if (!IsValidIndex(Index))
	{
		return;
	}

	// The same consumable on two slots means two bindings that do the same
	// thing, and the player having to remember which is which for no benefit.
	if (!ItemId.IsNone())
	{
		const int32 Existing = FindSlot(ItemId);
		if (Existing != INDEX_NONE && Existing != Index)
		{
			ClearSlot(Existing);
		}
	}

	Slots[Index] = ItemId;

	CachedItems.SetNum(Slots.Num());
	CachedItems[Index] = nullptr;
}

void UARPGQuickSlotComponent::ClearSlot(int32 Index)
{
	if (!IsValidIndex(Index))
	{
		return;
	}

	Slots[Index] = NAME_None;
	CachedItems.SetNum(Slots.Num());
	CachedItems[Index] = nullptr;
}

FName UARPGQuickSlotComponent::GetSlot(int32 Index) const
{
	return IsValidIndex(Index) ? Slots[Index] : NAME_None;
}

int32 UARPGQuickSlotComponent::FindSlot(FName ItemId) const
{
	if (ItemId.IsNone())
	{
		return INDEX_NONE;
	}
	return Slots.IndexOfByKey(ItemId);
}

int32 UARPGQuickSlotComponent::FindFreeSlot() const
{
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		if (Slots[Index].IsNone())
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

void UARPGQuickSlotComponent::SetSelectedIndex(int32 Index)
{
	if (!IsValidIndex(Index) || Index == SelectedIndex)
	{
		return;
	}

	SelectedIndex = Index;
	OnSelectionChanged.Broadcast(SelectedIndex);
}

int32 UARPGQuickSlotComponent::PeekSelection(int32 Direction) const
{
	if (Slots.Num() == 0 || Direction == 0)
	{
		return SelectedIndex;
	}

	const int32 Step = Direction > 0 ? 1 : -1;

	// Walks at most once around, so an entirely empty bar terminates rather than
	// spinning. Returning the current index in that case is what lets a HUD tell
	// "no neighbour" from "a neighbour that happens to be this one".
	for (int32 Offset = 1; Offset <= Slots.Num(); ++Offset)
	{
		const int32 Candidate =
			((SelectedIndex + Step * Offset) % Slots.Num() + Slots.Num()) % Slots.Num();
		if (!Slots[Candidate].IsNone())
		{
			return Candidate;
		}
	}

	return SelectedIndex;
}

void UARPGQuickSlotComponent::CycleSelection(int32 Direction)
{
	SetSelectedIndex(PeekSelection(Direction));
}

UARPGItemDefinition* UARPGQuickSlotComponent::GetSlotItem(int32 Index) const
{
	if (!IsValidIndex(Index) || Slots[Index].IsNone())
	{
		return nullptr;
	}

	CachedItems.SetNum(Slots.Num());

	if (const UARPGInventoryComponent* Inventory = GetInventory())
	{
		if (UARPGItemDefinition* Live = Inventory->FindItem(Slots[Index]))
		{
			CachedItems[Index] = Live;
			return Live;
		}
	}

	// Out of stock: fall back to whatever it last resolved to, so the slot can
	// still be drawn greyed rather than going blank the moment the player drinks
	// their last potion.
	return CachedItems[Index];
}

int32 UARPGQuickSlotComponent::GetSlotQuantity(int32 Index) const
{
	if (!IsValidIndex(Index) || Slots[Index].IsNone())
	{
		return 0;
	}

	const UARPGInventoryComponent* Inventory = GetInventory();
	return Inventory ? Inventory->GetItemQuantity(Slots[Index]) : 0;
}

bool UARPGQuickSlotComponent::UseSelected()
{
	if (!IsReady())
	{
		return false;
	}

	UARPGInventoryComponent* Inventory = GetInventory();
	UARPGItemDefinition* Item = GetSelectedItem();

	if (!Inventory || !Item || GetSelectedQuantity() <= 0)
	{
		return false;
	}

	UARPGConsumableDefinition* Consumable = Item->ConsumableDefinition;
	if (Item->ItemType != EARPGItemType::Consumable || !Consumable)
	{
		UE_LOG(LogARPGCombat, Warning,
			TEXT("Quick slot holds '%s', which is not a consumable."), *Item->ItemId.ToString());
		return false;
	}

	UAbilitySystemComponent* ASC =
		UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetOwner());
	if (!ASC)
	{
		return false;
	}

	// Spend FIRST. Applying the effects and then failing to decrement -- because
	// another system emptied the stack in between -- would be a free potion, and
	// the decrement is the operation that can actually fail.
	if (!Inventory->RemoveItem(Item->ItemId, 1))
	{
		return false;
	}

	for (const TSubclassOf<UGameplayEffect>& EffectClass : Consumable->Effects)
	{
		if (!EffectClass)
		{
			continue;
		}

		FGameplayEffectContextHandle ContextHandle = ASC->MakeEffectContext();
		ContextHandle.AddSourceObject(this);

		const FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(EffectClass, 1.f, ContextHandle);
		if (Spec.IsValid())
		{
			ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data);
		}
	}

	CooldownRemaining = Consumable->UseCooldown;
	OnConsumableUsed.Broadcast(Item);

	UE_LOG(LogARPGCombat, Log, TEXT("%s used '%s' (%d left)"),
		*GetNameSafe(GetOwner()), *Item->ItemId.ToString(),
		Inventory->GetItemQuantity(Item->ItemId));

	return true;
}

void UARPGQuickSlotComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (CooldownRemaining > 0.f)
	{
		CooldownRemaining = FMath::Max(0.f, CooldownRemaining - DeltaTime);
	}
}
