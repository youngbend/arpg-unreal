// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGProgressionTrackerComponent.h"
#include "ARPGXPCurve.h"

UARPGProgressionTrackerComponent::UARPGProgressionTrackerComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false); // progression is server state; UI reads it via RPC/save
}

void UARPGProgressionTrackerComponent::BeginPlay()
{
	Super::BeginPlay();

	MasteryWindow.SetCapacity(Settings ? Settings->MasteryWindow : 800);

	// Bound after begin play rather than in the constructor: the components a
	// tracker listens to are siblings, and none of them exist yet at construction.
	BindXPSource();
}

float UARPGProgressionTrackerComponent::GetMasteryFraction(FGameplayTag Subcomponent) const
{
	return MasteryWindow.GetFraction(Subcomponent);
}

int32 UARPGProgressionTrackerComponent::GetSubcomponentLevel(FGameplayTag Subcomponent) const
{
	const FARPGSubcomponentProgress* Progress = Subcomponents.Find(Subcomponent);
	return Progress ? Progress->BaseLevel : 0;
}

float UARPGProgressionTrackerComponent::GetEffectiveLevel(FGameplayTag Subcomponent) const
{
	if (bDebugForceMaxLevel)
	{
		return 4.f;
	}

	const FARPGSubcomponentProgress* Progress = Subcomponents.Find(Subcomponent);
	if (!Progress)
	{
		// Never used it: level 0, not level 1. An untrained element is one the
		// player cannot yet combine with anything, which is what the gate means.
		return 0.f;
	}

	const float XPPerLevel = Settings ? Settings->SubcomponentXPPerLevel : 500.f;
	return Progress->GetEffectiveLevel(MasteryWindow.GetFraction(Subcomponent), XPPerLevel);
}

float UARPGProgressionTrackerComponent::GetMultiplier(FGameplayTag Subcomponent) const
{
	return UARPGProgressionSettings::ComputeMultiplier(
		Settings, Category, GetEffectiveLevel(Subcomponent));
}

void UARPGProgressionTrackerComponent::RecordUse(FGameplayTag Subcomponent, float XP)
{
	if (!Subcomponent.IsValid())
	{
		return;
	}

	// Recorded regardless of XP. Mastery measures USE, so a maxed element still
	// has to be used to stay masterful -- gating this on XP would freeze the
	// window the moment proficiency capped.
	MasteryWindow.Record(Subcomponent);

	if (XP > 0.f)
	{
		const UARPGXPCurve* Curve = Settings ? Settings->CategoryXPCurve.Get() : nullptr;
		if (Category.AddXP(XP, Curve))
		{
			OnCategoryLevelUp.Broadcast(Category.Level, Category.TotalXP);
		}

		FARPGSubcomponentProgress& Progress = Subcomponents.FindOrAdd(Subcomponent);
		const float XPPerLevel = Settings ? Settings->SubcomponentXPPerLevel : 500.f;
		if (Progress.AddXP(XP, XPPerLevel))
		{
			OnSubcomponentLevelUp.Broadcast(Subcomponent, Progress.BaseLevel);
		}
	}
	else
	{
		// Still make the entry, so a subcomponent used but never rewarded reports
		// level 0 rather than "unknown" -- the two are the same number, but the
		// entry is what lets UI list what the player has actually touched.
		Subcomponents.FindOrAdd(Subcomponent);
	}
}

void UARPGProgressionTrackerComponent::RestoreProgress(const FARPGCategoryProgress& InCategory,
	const TMap<FGameplayTag, FARPGSubcomponentProgress>& InSubcomponents,
	const TArray<FGameplayTag>& MasteryHistory)
{
	Category = InCategory;
	Subcomponents = InSubcomponents;

	MasteryWindow.SetCapacity(Settings ? Settings->MasteryWindow : 800);
	MasteryWindow.SetHistory(MasteryHistory);
}
