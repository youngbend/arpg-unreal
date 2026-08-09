// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGProgressionTypes.h"
#include "ARPGMasteryWindow.h"
#include "ARPGXPCurve.h"

// ---------------------------------------------------------------------------
// XP curve
// ---------------------------------------------------------------------------

float UARPGXPCurve::GetXPToNext(int32 Level) const
{
	if (Level >= LevelCap)
	{
		// Effectively infinite rather than 0 or a flag: the loop that spends XP
		// then simply never clears the bar, so no caller needs a cap branch.
		return TNumericLimits<float>::Max();
	}

	return Base * FMath::Pow(static_cast<float>(FMath::Max(1, Level)), Exponent);
}

void UARPGXPCurve::SolveLevel(float TotalXP, int32& OutLevel, float& OutRemainder) const
{
	float Remaining = FMath::Max(0.f, TotalXP);
	OutLevel = 1;

	while (OutLevel < LevelCap)
	{
		const float Needed = GetXPToNext(OutLevel);
		if (Remaining < Needed)
		{
			break;
		}
		Remaining -= Needed;
		++OutLevel;
	}

	// No leftovers at the cap: there is no "into the next level" when there is
	// no next level, and reporting them leaves a capped bar looking partly full.
	OutRemainder = (OutLevel >= LevelCap) ? 0.f : Remaining;
}

int32 UARPGXPCurve::GetLevelForTotalXP(float TotalXP) const
{
	int32 Level = 1;
	float Remainder = 0.f;
	SolveLevel(TotalXP, Level, Remainder);
	return Level;
}

float UARPGXPCurve::GetRemainderForTotalXP(float TotalXP) const
{
	int32 Level = 1;
	float Remainder = 0.f;
	SolveLevel(TotalXP, Level, Remainder);
	return Remainder;
}

// ---------------------------------------------------------------------------
// Category
// ---------------------------------------------------------------------------

bool FARPGCategoryProgress::AddXP(float Amount, const UARPGXPCurve* Curve)
{
	if (Amount <= 0.f)
	{
		return false;
	}

	TotalXP += Amount;

	if (!Curve)
	{
		return false;
	}

	const int32 PreviousLevel = Level;

	// Re-solved from the TOTAL rather than incremented, so applying saved XP
	// lands on exactly the same level the player had -- an incremental path
	// would depend on the order XP arrived in.
	float Remainder = 0.f;
	Curve->SolveLevel(TotalXP, Level, Remainder);

	return Level > PreviousLevel;
}

float FARPGCategoryProgress::GetStepBonus(int32 TierSize, float BonusPerTier) const
{
	const int32 SafeTierSize = FMath::Max(1, TierSize);
	return FMath::FloorToFloat(static_cast<float>(Level) / SafeTierSize) * BonusPerTier;
}

// ---------------------------------------------------------------------------
// Subcomponent
// ---------------------------------------------------------------------------

bool FARPGSubcomponentProgress::AddXP(float Amount, float XPPerLevel)
{
	// Maxed: proficiency stops at 3 and mastery takes over from there, so extra
	// XP has nowhere to go rather than silently accumulating.
	if (BaseLevel >= 3 || Amount <= 0.f || XPPerLevel <= 0.f)
	{
		return false;
	}

	XPIntoLevel += Amount;

	bool bLevelled = false;
	while (BaseLevel < 3 && XPIntoLevel >= XPPerLevel)
	{
		XPIntoLevel -= XPPerLevel;
		++BaseLevel;
		bLevelled = true;
	}

	if (BaseLevel >= 3)
	{
		XPIntoLevel = 0.f;
	}

	return bLevelled;
}

float FARPGSubcomponentProgress::GetProficiencyPartial(float XPPerLevel) const
{
	if (BaseLevel >= 3 || XPPerLevel <= 0.f)
	{
		return static_cast<float>(FMath::Min(BaseLevel, 3));
	}

	return BaseLevel + FMath::Clamp(XPIntoLevel / XPPerLevel, 0.f, 1.f);
}

float FARPGSubcomponentProgress::GetEffectiveLevel(float MasteryFraction, float XPPerLevel) const
{
	if (BaseLevel < 3)
	{
		return GetProficiencyPartial(XPPerLevel);
	}

	// Never below 3. Proficiency is earned and permanent; mastery is current and
	// fades. Letting a decaying window drag the effective level back under 3
	// would re-lock combinations the player had already unlocked.
	return 3.f + FMath::Clamp(MasteryFraction, 0.f, 1.f);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

float UARPGProgressionSettings::ComputeMultiplier(const UARPGProgressionSettings* Settings,
	const FARPGCategoryProgress& Category, float EffectiveLevel)
{
	const int32 TierSize = Settings ? Settings->StepTierSize : 5;
	const float BonusPerTier = Settings ? Settings->StepBonusPerTier : 0.05f;
	const float W1 = Settings ? Settings->CategoryWeight : 1.f;
	const float W2 = Settings ? Settings->ProficiencyWeight : 1.f;
	const float W3 = Settings ? Settings->MasteryWeight : 1.f;

	const float Proficiency = FMath::Min(EffectiveLevel, 3.f) / 3.f;
	const float Mastery = FMath::Max(0.f, EffectiveLevel - 3.f);
	const float StepBonus = Category.GetStepBonus(TierSize, BonusPerTier);

	return (1.f + W1 * StepBonus) * (1.f + W2 * Proficiency) * (1.f + W3 * Mastery);
}

// ---------------------------------------------------------------------------
// Mastery window
// ---------------------------------------------------------------------------

void FARPGMasteryWindow::SetCapacity(int32 NewCapacity)
{
	Capacity = FMath::Max(1, NewCapacity);

	if (Events.Num() > Capacity)
	{
		// Keep the most RECENT events. Mastery is about what you have been doing
		// lately, so truncating the tail would be exactly backwards.
		Events.RemoveAt(0, Events.Num() - Capacity);
		RebuildCounts();
	}

	WriteIndex = Events.Num() % Capacity;
}

void FARPGMasteryWindow::Record(FGameplayTag SubcomponentTag)
{
	if (!SubcomponentTag.IsValid())
	{
		return;
	}

	if (Events.Num() < Capacity)
	{
		Events.Add(SubcomponentTag);
		Counts.FindOrAdd(SubcomponentTag)++;
		WriteIndex = Events.Num() % Capacity;
		return;
	}

	// Full: overwrite the oldest slot, decrementing whatever it held so the
	// counts stay exact rather than drifting.
	const FGameplayTag Evicted = Events[WriteIndex];
	if (int32* EvictedCount = Counts.Find(Evicted))
	{
		if (--(*EvictedCount) <= 0)
		{
			Counts.Remove(Evicted);
		}
	}

	Events[WriteIndex] = SubcomponentTag;
	Counts.FindOrAdd(SubcomponentTag)++;
	WriteIndex = (WriteIndex + 1) % Capacity;
}

float FARPGMasteryWindow::GetFraction(FGameplayTag SubcomponentTag) const
{
	if (Events.Num() == 0)
	{
		return 0.f;
	}

	const int32* Count = Counts.Find(SubcomponentTag);
	return Count ? static_cast<float>(*Count) / Events.Num() : 0.f;
}

void FARPGMasteryWindow::SetHistory(const TArray<FGameplayTag>& History)
{
	Events = History;

	if (Events.Num() > Capacity)
	{
		Events.RemoveAt(0, Events.Num() - Capacity);
	}

	WriteIndex = Events.Num() % Capacity;
	RebuildCounts();
}

void FARPGMasteryWindow::RebuildCounts()
{
	Counts.Reset();
	for (const FGameplayTag& Event : Events)
	{
		if (Event.IsValid())
		{
			Counts.FindOrAdd(Event)++;
		}
	}
}
