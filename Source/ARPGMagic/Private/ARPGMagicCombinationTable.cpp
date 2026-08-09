// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGMagicCombinationTable.h"
#include "ARPGMagic.h"
#include "ARPGMagicElement.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

bool UARPGMagicCombinationEntry::Matches(const FGameplayTagContainer& ActiveElements) const
{
	// Exact set equality, not containment. A row for fire+water must not fire
	// when the caster holds fire, water and air -- that is a three-way recipe
	// nobody has authored, and quietly resolving it as the two-way one would
	// make the third element vanish with no feedback.
	return ActiveElements.Num() == RequiredElements.Num()
		&& ActiveElements.HasAll(RequiredElements);
}

float UARPGMagicCombinationEntry::GetConsumptionRate(FGameplayTag ElementTag) const
{
	const float* Found = ConsumptionRates.Find(ElementTag);
	return Found ? FMath::Max(0.f, *Found) : 1.f;
}

FGameplayTag UARPGMagicCombinationEntry::GetAmplifiedElement() const
{
	if (!Result || !Result->ElementTag.IsValid())
	{
		return FGameplayTag();
	}

	return RequiredElements.HasTagExact(Result->ElementTag) ? Result->ElementTag : FGameplayTag();
}

bool UARPGMagicCombinationEntry::IsAmplifying() const
{
	return Mode == EARPGReactionMode::Auto && GetAmplifiedElement().IsValid();
}

FGameplayTag UARPGMagicCombinationEntry::GetConductedElement() const
{
	if (Mode != EARPGReactionMode::Conduct || !Result || RequiredElements.Num() != 2)
	{
		return FGameplayTag();
	}

	// The conducted element has to be one of the two, or the row says nothing
	// coherent about what actually travels.
	return RequiredElements.HasTagExact(Result->ElementTag) ? Result->ElementTag : FGameplayTag();
}

FGameplayTag UARPGMagicCombinationEntry::GetConductorElement() const
{
	const FGameplayTag Conducted = GetConductedElement();
	if (!Conducted.IsValid())
	{
		return FGameplayTag();
	}

	for (const FGameplayTag& Tag : RequiredElements)
	{
		if (Tag != Conducted)
		{
			return Tag;
		}
	}
	return FGameplayTag();
}

// ---------------------------------------------------------------------------

UARPGMagicElement* UARPGMagicCombinationTable::Resolve(
	const FGameplayTagContainer& ActiveElements, EARPGCombinationScope InScope) const
{
	const UARPGMagicCombinationEntry* Entry = ResolveEntry(ActiveElements, InScope);
	return Entry ? Entry->Result : nullptr;
}

UARPGMagicCombinationEntry* UARPGMagicCombinationTable::ResolveEntry(
	const FGameplayTagContainer& ActiveElements, EARPGCombinationScope InScope) const
{
	UARPGMagicCombinationEntry* Best = nullptr;
	int32 BestPriority = MIN_int32;

	for (UARPGMagicCombinationEntry* Entry : Entries)
	{
		if (!Entry || !Entry->AppliesTo(InScope) || !Entry->Matches(ActiveElements))
		{
			continue;
		}

		if (!Best || Entry->Priority > BestPriority)
		{
			Best = Entry;
			BestPriority = Entry->Priority;
		}
	}

	return Best;
}

#if WITH_EDITOR
EDataValidationResult UARPGMagicCombinationTable::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		const UARPGMagicCombinationEntry* Entry = Entries[Index];
		if (!Entry)
		{
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Entry %d is empty."), Index)));
			Result = EDataValidationResult::Invalid;
			continue;
		}

		if (Entry->RequiredElements.Num() < 2)
		{
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Entry %d needs at least two reactants; a one-element 'combination' would "
				     "fire the moment that element is readied alone."), Index)));
			Result = EDataValidationResult::Invalid;
		}

		if (!Entry->Result)
		{
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Entry %d has no Result, so it can match but produce nothing."), Index)));
			Result = EDataValidationResult::Invalid;
		}

		if (Entry->Scope == 0)
		{
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Entry %d applies to no scope, so it can never match anywhere."), Index)));
			Result = EDataValidationResult::Invalid;
		}

		// Conduction is the one mode that cannot be derived, so a row that
		// declares it and then does not describe a carrier is silently inert.
		if (Entry->Mode == EARPGReactionMode::Conduct && !Entry->GetConductedElement().IsValid())
		{
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Entry %d is Conduct but its Result is not one of its two reactants, so "
				     "nothing is described as travelling."), Index)));
			Result = EDataValidationResult::Invalid;
		}

		if (Entry->Mode == EARPGReactionMode::Solidify
			&& !Entry->AppliesTo(EARPGCombinationScope::Surface))
		{
			Context.AddWarning(FText::FromString(FString::Printf(
				TEXT("Entry %d is Solidify but is not in the Surface scope; there is nothing to "
				     "freeze without a body of fluid."), Index)));
		}

		for (const TPair<FGameplayTag, float>& Rate : Entry->ConsumptionRates)
		{
			if (!Entry->RequiredElements.HasTagExact(Rate.Key))
			{
				Context.AddWarning(FText::FromString(FString::Printf(
					TEXT("Entry %d has a consumption rate for '%s', which is not one of its "
					     "reactants; it will never be read."),
					Index, *Rate.Key.ToString())));
			}
		}
	}

	// Two rows matching the same set in the same scope at the same priority make
	// resolution depend on array order, which is not something anyone authored.
	for (int32 A = 0; A < Entries.Num(); ++A)
	{
		for (int32 B = A + 1; B < Entries.Num(); ++B)
		{
			const UARPGMagicCombinationEntry* First = Entries[A];
			const UARPGMagicCombinationEntry* Second = Entries[B];
			if (!First || !Second || First->Priority != Second->Priority)
			{
				continue;
			}

			const bool bSameSet = First->RequiredElements.Num() == Second->RequiredElements.Num()
				&& First->RequiredElements.HasAll(Second->RequiredElements);

			if (bSameSet && (First->Scope & Second->Scope) != 0)
			{
				Context.AddWarning(FText::FromString(FString::Printf(
					TEXT("Entries %d and %d match the same elements in an overlapping scope at the "
					     "same priority; which one wins depends on array order. Give one a higher "
					     "Priority."), A, B)));
			}
		}
	}

	return Result;
}
#endif
