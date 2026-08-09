// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "ARPGMagicLoadout.generated.h"

class UARPGMagicElement;

/** The four face buttons, in the order the loadout stores them. */
UENUM(BlueprintType)
enum class EARPGElementSlot : uint8
{
	North = 0,
	West  = 1,
	South = 2,
	East  = 3,

	MAX UMETA(Hidden)
};

/**
 * One character's equipped elements. Port of Godot's MagicLoadout.
 *
 * Organised as PAGES of four slots, one slot per face button, with the player
 * switching pages at runtime. Combination recipes are not stored here -- they
 * live in the combination table, because a recipe is a property of the elements
 * rather than of who happens to have them equipped.
 */
UCLASS(BlueprintType)
class ARPGMAGIC_API UARPGMagicLoadout : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	static constexpr int32 SlotCount = static_cast<int32>(EARPGElementSlot::MAX);

	/**
	 * Flat, of size PageCount * SlotCount, indexed Page * SlotCount + Slot.
	 *
	 * Flat rather than an array of page structs so the whole loadout is one
	 * editable list in the details panel, matching how it was authored in Godot.
	 * Use the accessors -- they range-check, which raw indexing into this does
	 * not.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Loadout")
	TArray<TObjectPtr<UARPGMagicElement>> Elements;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Loadout",
		meta = (ClampMin = "1"))
	int32 PageCount = 1;

	/**
	 * How many elements may be active at once. Starts at 2 and rises as the
	 * player unlocks more complex spellcasting.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Loadout",
		meta = (ClampMin = "1", ClampMax = "4"))
	int32 MaxElements = 2;

	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	UARPGMagicElement* GetSlot(int32 Page, EARPGElementSlot Slot) const;

	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	void SetSlot(int32 Page, EARPGElementSlot Slot, UARPGMagicElement* Element);

	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	bool IsSlotFilled(int32 Page, EARPGElementSlot Slot) const
	{
		return GetSlot(Page, Slot) != nullptr;
	}

	/** The elements a 4-bit active mask selects on this page, skipping empty slots. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	TArray<UARPGMagicElement*> GetActiveElements(int32 Page, int32 ActiveMask) const;

	/** Their tags, which is what the combination table matches on. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	FGameplayTagContainer GetActiveElementTags(int32 Page, int32 ActiveMask) const;

	/**
	 * Counts SET BITS, not filled slots -- a mask bit for an empty slot still
	 * counts against the limit.
	 *
	 * That is deliberate and matches the Godot behaviour: the mask is what the
	 * player pressed, and letting an empty slot be free would make the cap
	 * depend on loadout gaps.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	static int32 ActiveCount(int32 ActiveMask);

	/** Resizes Elements to match PageCount, preserving what fits. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	void ConformToPageCount();

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGMagicLoadout", GetFName());
	}

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** Flat index, or INDEX_NONE when the page or slot is out of range. */
	int32 FlatIndex(int32 Page, EARPGElementSlot Slot) const;
};
