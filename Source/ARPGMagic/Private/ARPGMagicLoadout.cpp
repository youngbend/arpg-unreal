// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGMagicLoadout.h"
#include "ARPGMagic.h"
#include "ARPGMagicElement.h"

int32 UARPGMagicLoadout::FlatIndex(int32 Page, EARPGElementSlot Slot) const
{
	const int32 SlotIndex = static_cast<int32>(Slot);
	if (Page < 0 || Page >= PageCount || SlotIndex < 0 || SlotIndex >= SlotCount)
	{
		return INDEX_NONE;
	}

	const int32 Index = Page * SlotCount + SlotIndex;
	return Elements.IsValidIndex(Index) ? Index : INDEX_NONE;
}

UARPGMagicElement* UARPGMagicLoadout::GetSlot(int32 Page, EARPGElementSlot Slot) const
{
	const int32 Index = FlatIndex(Page, Slot);
	return Index != INDEX_NONE ? Elements[Index] : nullptr;
}

void UARPGMagicLoadout::SetSlot(int32 Page, EARPGElementSlot Slot, UARPGMagicElement* Element)
{
	// Grow to fit rather than refusing: setting a slot on a page that exists but
	// has not been sized yet is the normal path when building a loadout in code.
	if (Page >= 0 && Page < PageCount)
	{
		ConformToPageCount();
	}

	const int32 Index = FlatIndex(Page, Slot);
	if (Index == INDEX_NONE)
	{
		UE_LOG(LogARPGMagic, Warning,
			TEXT("SetSlot(%d, %d) is outside a loadout of %d page(s)."),
			Page, static_cast<int32>(Slot), PageCount);
		return;
	}

	Elements[Index] = Element;
}

TArray<UARPGMagicElement*> UARPGMagicLoadout::GetActiveElements(int32 Page, int32 ActiveMask) const
{
	TArray<UARPGMagicElement*> Active;
	for (int32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
	{
		if ((ActiveMask & (1 << SlotIndex)) == 0)
		{
			continue;
		}

		if (UARPGMagicElement* Element = GetSlot(Page, static_cast<EARPGElementSlot>(SlotIndex)))
		{
			Active.Add(Element);
		}
	}
	return Active;
}

FGameplayTagContainer UARPGMagicLoadout::GetActiveElementTags(int32 Page, int32 ActiveMask) const
{
	FGameplayTagContainer Tags;
	for (const UARPGMagicElement* Element : GetActiveElements(Page, ActiveMask))
	{
		if (Element->ElementTag.IsValid())
		{
			Tags.AddTag(Element->ElementTag);
		}
	}
	return Tags;
}

int32 UARPGMagicLoadout::ActiveCount(int32 ActiveMask)
{
	int32 Count = 0;
	for (int32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
	{
		if (ActiveMask & (1 << SlotIndex))
		{
			++Count;
		}
	}
	return Count;
}

void UARPGMagicLoadout::ConformToPageCount()
{
	const int32 Required = FMath::Max(1, PageCount) * SlotCount;
	if (Elements.Num() != Required)
	{
		Elements.SetNum(Required);
	}
}

#if WITH_EDITOR
void UARPGMagicLoadout::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Keeps the flat array in step with the page count, so adding a page in the
	// details panel produces four editable slots rather than a size mismatch
	// that silently drops the last page.
	const FName Changed = PropertyChangedEvent.GetPropertyName();
	if (Changed == GET_MEMBER_NAME_CHECKED(UARPGMagicLoadout, PageCount))
	{
		ConformToPageCount();
	}
}
#endif
