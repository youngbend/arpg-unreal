// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "ARPGTestFixtures.h"

#if WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS

#include "ARPGCombatDummy.h"
#include "ARPGConsumableDefinition.h"
#include "ARPGGameplayTags.h"
#include "ARPGInventoryComponent.h"
#include "ARPGItemDefinition.h"
#include "ARPGMagicCaster.h"
#include "ARPGMagicComponent.h"
#include "ARPGMagicElement.h"
#include "ARPGMagicLoadout.h"
#include "ARPGMagicProgressionTracker.h"
#include "ARPGProgressionComponent.h"
#include "ARPGProgressionTypes.h"
#include "ARPGQuickSlotComponent.h"
#include "ARPGVitalSet.h"
#include "ARPGXPCurve.h"
#include "AbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

/**
 * Progression and inventory.
 *
 * The two things worth protecting here are ARITHMETIC and IDEMPOTENCE. The
 * formulas are pure functions of authored numbers, so they can be pinned
 * exactly; and allocation has to survive being re-applied on every load, which
 * is the failure that is invisible in one session and unbounded across many.
 */
namespace ARPGProgressionTestUtils
{
	using ARPGTest::FTestWorld;

	void Tick(UActorComponent* Component, float Seconds, float Step = 0.05f)
	{
		for (float Elapsed = 0.f; Elapsed < Seconds; Elapsed += Step)
		{
			Component->TickComponent(Step, LEVELTICK_All, nullptr);
		}
	}

	UARPGXPCurve* MakeCurve(UObject* Outer, float Base = 100.f, float Exponent = 1.f, int32 Cap = 50)
	{
		UARPGXPCurve* Curve = NewObject<UARPGXPCurve>(Outer);
		Curve->Base = Base;
		Curve->Exponent = Exponent;
		Curve->LevelCap = Cap;
		return Curve;
	}

	UARPGItemDefinition* MakeItem(UObject* Outer, FName Id, int32 MaxStack,
		EARPGItemType Type = EARPGItemType::Material)
	{
		UARPGItemDefinition* Item = NewObject<UARPGItemDefinition>(Outer);
		Item->ItemId = Id;
		Item->MaxStackSize = MaxStack;
		Item->ItemType = Type;
		return Item;
	}
}

// ---------------------------------------------------------------------------
// The curve
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGXPCurveTest,
	"ARPG.Progression.Curve.SolvesLevelFromTotalXP",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGXPCurveTest::RunTest(const FString& Parameters)
{
	using namespace ARPGProgressionTestUtils;

	// Linear: level 1->2 costs 100, 2->3 costs 200, 3->4 costs 300.
	UARPGXPCurve* Curve = MakeCurve(GetTransientPackage(), 100.f, 1.f, 4);

	TestEqual(TEXT("First level costs the base"), Curve->GetXPToNext(1), 100.f);
	TestEqual(TEXT("And each one after costs more"), Curve->GetXPToNext(2), 200.f);

	TestEqual(TEXT("No XP is level 1"), Curve->GetLevelForTotalXP(0.f), 1);
	TestEqual(TEXT("Just short of the first level-up"), Curve->GetLevelForTotalXP(99.f), 1);
	TestEqual(TEXT("Exactly enough levels up"), Curve->GetLevelForTotalXP(100.f), 2);
	TestEqual(TEXT("And the remainder resets"), Curve->GetRemainderForTotalXP(100.f), 0.f);
	TestEqual(TEXT("Partway into the second"), Curve->GetRemainderForTotalXP(150.f), 50.f);
	TestEqual(TEXT("300 buys two levels"), Curve->GetLevelForTotalXP(300.f), 3);

	// At the cap the bar stops rather than filling forever.
	TestEqual(TEXT("Enormous XP still caps"), Curve->GetLevelForTotalXP(999999.f), 4);
	TestEqual(TEXT("And reports no leftovers, so a capped bar is not shown part-full"),
		Curve->GetRemainderForTotalXP(999999.f), 0.f);

	return true;
}

// ---------------------------------------------------------------------------
// The multiplier
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMultiplierTest,
	"ARPG.Progression.Multiplier.ThreeAxesCompoundIndependently",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMultiplierTest::RunTest(const FString& Parameters)
{
	using namespace ARPGProgressionTestUtils;

	UARPGProgressionSettings* Settings = NewObject<UARPGProgressionSettings>();
	Settings->StepTierSize = 5;
	Settings->StepBonusPerTier = 0.1f;

	FARPGCategoryProgress Category;
	Category.Level = 1;

	// Untrained, un-levelled: no bonus at all. A multiplier that started above 1
	// would make every authored damage number a lie.
	TestEqual(TEXT("Nothing earned is no bonus"),
		UARPGProgressionSettings::ComputeMultiplier(Settings, Category, 0.f), 1.f);

	// Proficiency alone: level 3 of 3 doubles.
	TestEqual(TEXT("Full proficiency doubles"),
		UARPGProgressionSettings::ComputeMultiplier(Settings, Category, 3.f), 2.f);

	// Mastery alone, on top of full proficiency: 4.0 is prof 1 and mastery 1.
	TestEqual(TEXT("Full mastery doubles again"),
		UARPGProgressionSettings::ComputeMultiplier(Settings, Category, 4.f), 4.f);

	// Category level 5 crosses one tier: +0.1, applied multiplicatively.
	Category.Level = 5;
	TestEqual(TEXT("A category tier multiplies the whole thing"),
		UARPGProgressionSettings::ComputeMultiplier(Settings, Category, 3.f), 2.2f, 0.001f);

	// The step is a STEP: level 9 is still one tier, level 10 is two.
	Category.Level = 9;
	TestEqual(TEXT("Partway to the next tier is still one tier"),
		UARPGProgressionSettings::ComputeMultiplier(Settings, Category, 0.f), 1.1f, 0.001f);
	Category.Level = 10;
	TestEqual(TEXT("Crossing it is a discrete jump"),
		UARPGProgressionSettings::ComputeMultiplier(Settings, Category, 0.f), 1.2f, 0.001f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSubcomponentTest,
	"ARPG.Progression.Subcomponent.MasteryNeverDragsBelowEarnedProficiency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSubcomponentTest::RunTest(const FString& Parameters)
{
	FARPGSubcomponentProgress Progress;
	const float XPPerLevel = 100.f;

	TestEqual(TEXT("Untrained is 0"), Progress.GetEffectiveLevel(0.f, XPPerLevel), 0.f);

	Progress.AddXP(50.f, XPPerLevel);
	TestEqual(TEXT("Half a level reads as a partial"),
		Progress.GetEffectiveLevel(0.f, XPPerLevel), 0.5f);

	TestTrue(TEXT("Enough XP levels up"), Progress.AddXP(50.f, XPPerLevel));
	TestEqual(TEXT("And lands exactly on the level"),
		Progress.GetEffectiveLevel(0.f, XPPerLevel), 1.f);

	// A single large grant crosses several levels rather than banking the excess.
	Progress.AddXP(500.f, XPPerLevel);
	TestEqual(TEXT("Proficiency caps at 3"), Progress.BaseLevel, 3);

	// Above 3 the level is 3 + the current mastery window, and mastery FADES.
	TestEqual(TEXT("Maxed with no recent use is exactly 3"),
		Progress.GetEffectiveLevel(0.f, XPPerLevel), 3.f);
	TestEqual(TEXT("Half the window is 3.5"),
		Progress.GetEffectiveLevel(0.5f, XPPerLevel), 3.5f);

	// The floor is the whole point: proficiency is earned and permanent, mastery
	// is current and fades. Without it, a decaying window would silently re-lock
	// combinations the player had already unlocked.
	TestEqual(TEXT("An empty window never drags below what was earned"),
		Progress.GetEffectiveLevel(0.f, XPPerLevel), 3.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMasteryWindowTest,
	"ARPG.Progression.Mastery.WindowIsAFractionOfRecentUse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMasteryWindowTest::RunTest(const FString& Parameters)
{
	FARPGMasteryWindow Window;
	Window.SetCapacity(4);

	TestEqual(TEXT("An empty window is zero, not undefined"),
		Window.GetFraction(TAG_Element_Fire), 0.f);

	Window.Record(TAG_Element_Fire);
	Window.Record(TAG_Element_Fire);
	TestEqual(TEXT("All fire so far is all of the window"),
		Window.GetFraction(TAG_Element_Fire), 1.f);

	Window.Record(TAG_Element_Water);
	Window.Record(TAG_Element_Water);
	TestEqual(TEXT("Half and half"), Window.GetFraction(TAG_Element_Fire), 0.5f);

	// Full: the oldest events fall out. THIS is what makes mastery fade -- it is
	// a fraction of RECENT use, not a running total.
	Window.Record(TAG_Element_Water);
	Window.Record(TAG_Element_Water);
	TestEqual(TEXT("Fire has aged out of the window"),
		Window.GetFraction(TAG_Element_Fire), 0.f);
	TestEqual(TEXT("Leaving water at full"),
		Window.GetFraction(TAG_Element_Water), 1.f);

	// Save/load round trip.
	const TArray<FGameplayTag> History = Window.GetHistory();
	FARPGMasteryWindow Restored;
	Restored.SetCapacity(4);
	Restored.SetHistory(History);
	TestEqual(TEXT("A restored window reports the same fraction"),
		Restored.GetFraction(TAG_Element_Water), 1.f);

	return true;
}

// ---------------------------------------------------------------------------
// The visible level
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGProgressionAllocationTest,
	"ARPG.Progression.Level.AllocationIsIdempotent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGProgressionAllocationTest::RunTest(const FString& Parameters)
{
	using namespace ARPGProgressionTestUtils;
	FTestWorld Scope;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AARPGCombatDummy* Actor = Scope.World->SpawnActor<AARPGCombatDummy>(
		AARPGCombatDummy::StaticClass(), FTransform::Identity, Params);

	UAbilitySystemComponent* ASC = Actor->GetAbilitySystemComponent();
	ASC->InitAbilityActorInfo(Actor, Actor);
	ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxHealthAttribute(), 100.f);

	UARPGProgressionComponent* Progression = NewObject<UARPGProgressionComponent>(Actor);
	Progression->XPCurve = MakeCurve(Actor, 100.f, 1.f, 10);
	Progression->HealthPerPoint = 10.f;
	Progression->PointsPerLevel = 1;
	Progression->RegisterComponent();

	Progression->GrantXP(100.f);
	TestEqual(TEXT("Enough XP levels up"), Progression->GetLevel(), 2);
	TestEqual(TEXT("And grants a point"), Progression->GetAvailablePoints(), 1);

	TestTrue(TEXT("Which can be spent"), Progression->SpendPoint(EARPGAttributePoint::Health));
	TestEqual(TEXT("Raising max health"),
		ASC->GetNumericAttribute(UARPGVitalSet::GetMaxHealthAttribute()), 110.f);
	TestFalse(TEXT("But only once"), Progression->SpendPoint(EARPGAttributePoint::Health));

	// THE REGRESSION THIS EXISTS FOR. Restoring a save re-applies allocations,
	// and a delta-based implementation would add the point again every load --
	// invisible in one session, unbounded across many.
	Progression->RestoreProgress(2, 100.f, 0, 1, 0, 0);
	TestEqual(TEXT("Re-applying a save lands on the same number"),
		ASC->GetNumericAttribute(UARPGVitalSet::GetMaxHealthAttribute()), 110.f);

	Progression->RestoreProgress(2, 100.f, 0, 1, 0, 0);
	TestEqual(TEXT("And again, however many times it is loaded"),
		ASC->GetNumericAttribute(UARPGVitalSet::GetMaxHealthAttribute()), 110.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGProgressionMultiLevelTest,
	"ARPG.Progression.Level.OneBigGrantAwardsEveryLevel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGProgressionMultiLevelTest::RunTest(const FString& Parameters)
{
	using namespace ARPGProgressionTestUtils;
	FTestWorld Scope;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AARPGCombatDummy* Actor = Scope.World->SpawnActor<AARPGCombatDummy>(
		AARPGCombatDummy::StaticClass(), FTransform::Identity, Params);
	Actor->GetAbilitySystemComponent()->InitAbilityActorInfo(Actor, Actor);

	UARPGProgressionComponent* Progression = NewObject<UARPGProgressionComponent>(Actor);
	Progression->XPCurve = MakeCurve(Actor, 100.f, 1.f, 10);
	Progression->PointsPerLevel = 2;
	Progression->RegisterComponent();

	// 100 + 200 = two level-ups in one grant. Stepping one level at a time would
	// award only the first, quietly swallowing a boss's worth of XP.
	Progression->GrantXP(300.f);

	TestEqual(TEXT("A big grant crosses several levels"), Progression->GetLevel(), 3);
	TestEqual(TEXT("And pays for all of them"), Progression->GetAvailablePoints(), 4);

	return true;
}

// ---------------------------------------------------------------------------
// Mastery reaching magic
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGTrackerDrivesMagicTest,
	"ARPG.Progression.Tracker.MasteryMultipliesDischargeDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGTrackerDrivesMagicTest::RunTest(const FString& Parameters)
{
	using namespace ARPGProgressionTestUtils;
	FTestWorld Scope;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AARPGMagicCaster* Caster = Scope.World->SpawnActor<AARPGMagicCaster>(
		AARPGMagicCaster::StaticClass(), FTransform::Identity, Params);

	UAbilitySystemComponent* ASC = Caster->GetAbilitySystemComponent();
	ASC->InitAbilityActorInfo(Caster, Caster);
	ASC->SetNumericAttributeBase(UARPGVitalSet::GetManaAttribute(), 100.f);

	UARPGMagicLoadout* Loadout = NewObject<UARPGMagicLoadout>();
	Loadout->PageCount = 1;
	Loadout->ConformToPageCount();

	UARPGMagicElement* Fire = NewObject<UARPGMagicElement>(Loadout);
	Fire->ElementTag = TAG_Element_Fire;
	Fire->BaseDamage = 20.f;
	Fire->ActivationCost = 0.f;
	Loadout->SetSlot(0, EARPGElementSlot::North, Fire);

	UARPGMagicComponent* Magic = Caster->Magic;
	Magic->Loadout = Loadout;

	FARPGDischargeTypeSettings Burst;
	Burst.DamageMultiplier = 1.f;
	Burst.MaxChargeDamage = 0.f;
	Magic->DischargeSettings.Add(EARPGDischargeType::Burst, Burst);

	UARPGProgressionSettings* Settings = NewObject<UARPGProgressionSettings>();
	Settings->SubcomponentXPPerLevel = 100.f;
	Settings->CategoryXPCurve = MakeCurve(Settings, 1000.f, 1.f, 50);
	Settings->StepBonusPerTier = 0.f; // isolate proficiency from the category step

	UARPGMagicProgressionTracker* Tracker = NewObject<UARPGMagicProgressionTracker>(Caster);
	Tracker->Settings = Settings;
	Tracker->RegisterComponent();

	Magic->ToggleElement(EARPGElementSlot::North);

	// Untrained: the multiplier is 1, so the authored number is what lands. The
	// tracker COMPONENT must win over the caster actor, which also implements
	// the interface -- otherwise the real progression system is shadowed by the
	// test fixture's stub.
	const FARPGDischargeContext Untrained = Magic->BuildDischargeContext(
		EARPGDischargeType::Burst, FVector::ZeroVector, FVector::ForwardVector, 0.f);
	TestEqual(TEXT("Untrained deals the authored damage"), Untrained.ComputedDamage, 20.f);

	// Train fire to full proficiency: 3 levels at 100 XP each.
	Tracker->RecordUse(TAG_Element_Fire, 300.f);
	TestEqual(TEXT("Setup: fire is maxed"), Tracker->GetSubcomponentLevel(TAG_Element_Fire), 3);

	const FARPGDischargeContext Trained = Magic->BuildDischargeContext(
		EARPGDischargeType::Burst, FVector::ZeroVector, FVector::ForwardVector, 0.f);

	// Every recorded use was fire, so the mastery window is entirely fire:
	// effective level 4, which is (1 + 1) * (1 + 1) = 4x.
	TestEqual(TEXT("Mastery multiplies discharge damage"), Trained.ComputedDamage, 80.f, 0.01f);

	// And poise deliberately does NOT inherit it -- the phase 5 invariant still
	// holds now that a real tracker is supplying the numbers.
	TestEqual(TEXT("But leaves poise alone"), Trained.ComputedPoiseDamage, 0.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGTrackerGatesTest,
	"ARPG.Progression.Tracker.UntrainedCasterIsGated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGTrackerGatesTest::RunTest(const FString& Parameters)
{
	using namespace ARPGProgressionTestUtils;
	FTestWorld Scope;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AARPGMagicCaster* Caster = Scope.World->SpawnActor<AARPGMagicCaster>(
		AARPGMagicCaster::StaticClass(), FTransform::Identity, Params);
	Caster->GetAbilitySystemComponent()->InitAbilityActorInfo(Caster, Caster);

	UARPGMagicLoadout* Loadout = NewObject<UARPGMagicLoadout>();
	Loadout->PageCount = 1;
	Loadout->MaxElements = 2;
	Loadout->ConformToPageCount();

	UARPGMagicElement* Fire = NewObject<UARPGMagicElement>(Loadout);
	Fire->ElementTag = TAG_Element_Fire;
	Fire->Complexity = 2;
	Fire->ActivationCost = 0.f;

	UARPGMagicElement* Water = NewObject<UARPGMagicElement>(Loadout);
	Water->ElementTag = TAG_Element_Water;
	Water->Complexity = 2;
	Water->ActivationCost = 0.f;

	Loadout->SetSlot(0, EARPGElementSlot::North, Fire);
	Loadout->SetSlot(0, EARPGElementSlot::West, Water);

	Caster->Magic->Loadout = Loadout;

	UARPGProgressionSettings* Settings = NewObject<UARPGProgressionSettings>();
	Settings->SubcomponentXPPerLevel = 100.f;

	UARPGMagicProgressionTracker* Tracker = NewObject<UARPGMagicProgressionTracker>(Caster);
	Tracker->Settings = Settings;
	Tracker->RegisterComponent();

	Caster->Magic->ToggleElement(EARPGElementSlot::North);
	Caster->Magic->ToggleElement(EARPGElementSlot::West);

	// A tracker present and empty is meaningfully different from no tracker: an
	// untrained caster is GATED, while a caster with no progression system is
	// exempt. Both cases now have a test.
	TestEqual(TEXT("An untrained caster cannot combine"), Caster->Magic->GetActiveCount(), 1);

	Tracker->RecordUse(TAG_Element_Fire, 200.f);
	Tracker->RecordUse(TAG_Element_Water, 200.f);

	Caster->Magic->ClearSelection();
	Caster->Magic->ToggleElement(EARPGElementSlot::North);
	Caster->Magic->ToggleElement(EARPGElementSlot::West);

	TestEqual(TEXT("Once trained in both, the pair is allowed"),
		Caster->Magic->GetActiveCount(), 2);

	return true;
}

// ---------------------------------------------------------------------------
// Inventory
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGInventoryStackTest,
	"ARPG.Inventory.Stacking.FillsExistingStacksFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGInventoryStackTest::RunTest(const FString& Parameters)
{
	using namespace ARPGProgressionTestUtils;
	FTestWorld Scope;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Actor = Scope.World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);

	UARPGInventoryComponent* Inventory = NewObject<UARPGInventoryComponent>(Actor);
	Inventory->Capacity = 2;
	Inventory->RegisterComponent();

	UARPGItemDefinition* Potion = MakeItem(Actor, TEXT("potion"), 5);

	TestEqual(TEXT("Adding fits in one slot"), Inventory->AddItem(Potion, 3), 3);
	TestEqual(TEXT("Using one slot"), Inventory->GetUsedSlots(), 1);

	// Tops up the existing stack before opening another: two half-full stacks of
	// the same thing is what a bag should never end up with.
	TestEqual(TEXT("More tops up the same stack"), Inventory->AddItem(Potion, 2), 2);
	TestEqual(TEXT("Still one slot"), Inventory->GetUsedSlots(), 1);
	TestEqual(TEXT("Holding a full stack"), Inventory->GetItemQuantity(TEXT("potion")), 5);

	TestEqual(TEXT("The overflow opens a second slot"), Inventory->AddItem(Potion, 3), 3);
	TestEqual(TEXT("Which is now used"), Inventory->GetUsedSlots(), 2);

	// Capacity counts SLOTS. Full at 2 slots, so only the remaining stack space
	// can be filled -- and the partial add is reported rather than refused.
	TestEqual(TEXT("A full bag takes only what fits"), Inventory->AddItem(Potion, 10), 2);
	TestEqual(TEXT("Reaching capacity exactly"), Inventory->GetItemQuantity(TEXT("potion")), 10);
	TestEqual(TEXT("And nothing more fits"), Inventory->AddItem(Potion, 1), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGInventoryRemoveTest,
	"ARPG.Inventory.Stacking.RemovalIsAllOrNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGInventoryRemoveTest::RunTest(const FString& Parameters)
{
	using namespace ARPGProgressionTestUtils;
	FTestWorld Scope;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Actor = Scope.World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);

	UARPGInventoryComponent* Inventory = NewObject<UARPGInventoryComponent>(Actor);
	Inventory->RegisterComponent();

	UARPGItemDefinition* Potion = MakeItem(Actor, TEXT("potion"), 5);
	Inventory->AddItem(Potion, 7); // two slots: 5 and 2

	// All or nothing, unlike adding: a partial removal would leave the caller
	// believing it had spent something it had not.
	TestFalse(TEXT("Asking for more than is held removes nothing"),
		Inventory->RemoveItem(TEXT("potion"), 8));
	TestEqual(TEXT("Leaving the bag untouched"), Inventory->GetItemQuantity(TEXT("potion")), 7);

	TestTrue(TEXT("Removing what is held succeeds"), Inventory->RemoveItem(TEXT("potion"), 3));
	TestEqual(TEXT("Spending across stacks"), Inventory->GetItemQuantity(TEXT("potion")), 4);
	TestEqual(TEXT("And collapsing the emptied one"), Inventory->GetUsedSlots(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGInventoryInstanceTest,
	"ARPG.Inventory.Stacking.InstanceIdsAreStableAndUnique",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGInventoryInstanceTest::RunTest(const FString& Parameters)
{
	using namespace ARPGProgressionTestUtils;
	FTestWorld Scope;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Actor = Scope.World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);

	UARPGInventoryComponent* Inventory = NewObject<UARPGInventoryComponent>(Actor);
	Inventory->RegisterComponent();

	UARPGItemDefinition* Potion = MakeItem(Actor, TEXT("potion"), 5);
	Inventory->AddItem(Potion, 7);

	const TArray<FARPGInventorySlot>& Slots = Inventory->GetSlots();
	TestEqual(TEXT("Setup: two stacks"), Slots.Num(), 2);

	const int32 FirstId = Slots[0].InstanceId;
	const int32 SecondId = Slots[1].InstanceId;
	TestNotEqual(TEXT("Stacks of the same item are distinguishable"), FirstId, SecondId);

	// Naming ONE stack is why instance ids exist: an index would be invalidated
	// by any removal before it, and the item alone cannot say which stack.
	TestTrue(TEXT("A named stack can be spent alone"),
		Inventory->RemoveFromSlot(SecondId, 2));
	TestEqual(TEXT("Emptying it"), Inventory->GetUsedSlots(), 1);
	TestEqual(TEXT("The other stack is untouched"),
		Inventory->GetItemQuantity(TEXT("potion")), 5);
	TestEqual(TEXT("And keeps its id across the other's removal"),
		Inventory->GetSlots()[0].InstanceId, FirstId);

	return true;
}

// ---------------------------------------------------------------------------
// Quick slots
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGQuickSlotCycleTest,
	"ARPG.Inventory.QuickSlots.CyclingSkipsEmptySlots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGQuickSlotCycleTest::RunTest(const FString& Parameters)
{
	using namespace ARPGProgressionTestUtils;
	FTestWorld Scope;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Actor = Scope.World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);

	UARPGQuickSlotComponent* Bar = NewObject<UARPGQuickSlotComponent>(Actor);
	Bar->RegisterComponent();
	Bar->SetSlotCount(4);

	Bar->AssignSlot(0, TEXT("potion"));
	Bar->AssignSlot(2, TEXT("ether"));

	// Slots 1 and 3 are empty, so cycling steps straight over them: paging
	// through blanks is the behaviour of a bar that does not know what is on it.
	Bar->CycleSelection(1);
	TestEqual(TEXT("Cycling skips the empty slot"), Bar->GetSelectedIndex(), 2);

	Bar->CycleSelection(1);
	TestEqual(TEXT("And wraps around the end"), Bar->GetSelectedIndex(), 0);

	Bar->CycleSelection(-1);
	TestEqual(TEXT("Going the other way wraps too"), Bar->GetSelectedIndex(), 2);

	// The same consumable must not occupy two slots.
	Bar->AssignSlot(1, TEXT("potion"));
	TestEqual(TEXT("Re-assigning moves the binding"), Bar->GetSlot(1), FName(TEXT("potion")));
	TestEqual(TEXT("Rather than duplicating it"), Bar->GetSlot(0), FName(NAME_None));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGQuickSlotUseTest,
	"ARPG.Inventory.QuickSlots.UsingSpendsOneAndStartsTheCooldown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGQuickSlotUseTest::RunTest(const FString& Parameters)
{
	using namespace ARPGProgressionTestUtils;
	FTestWorld Scope;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AARPGCombatDummy* Actor = Scope.World->SpawnActor<AARPGCombatDummy>(
		AARPGCombatDummy::StaticClass(), FTransform::Identity, Params);
	Actor->GetAbilitySystemComponent()->InitAbilityActorInfo(Actor, Actor);

	UARPGInventoryComponent* Inventory = NewObject<UARPGInventoryComponent>(Actor);
	Inventory->RegisterComponent();

	UARPGConsumableDefinition* PotionEffects = NewObject<UARPGConsumableDefinition>(Actor);
	PotionEffects->ConsumableId = TEXT("potion");
	PotionEffects->UseCooldown = 1.f;

	UARPGItemDefinition* Potion = MakeItem(Actor, TEXT("potion"), 10, EARPGItemType::Consumable);
	Potion->ConsumableDefinition = PotionEffects;
	Inventory->AddItem(Potion, 2);

	UARPGQuickSlotComponent* Bar = NewObject<UARPGQuickSlotComponent>(Actor);
	Bar->RegisterComponent();
	Bar->SetSlotCount(2);
	Bar->AssignSlot(0, TEXT("potion"));

	TestEqual(TEXT("Setup: the slot resolves its item"), Bar->GetSelectedQuantity(), 2);

	TestTrue(TEXT("Using succeeds"), Bar->UseSelected());
	TestEqual(TEXT("And spends exactly one"), Inventory->GetItemQuantity(TEXT("potion")), 1);
	TestFalse(TEXT("The cooldown blocks an immediate second"), Bar->UseSelected());
	TestEqual(TEXT("Which spent nothing"), Inventory->GetItemQuantity(TEXT("potion")), 1);

	Tick(Bar, 1.2f);
	TestTrue(TEXT("Once it expires, using works again"), Bar->UseSelected());
	TestEqual(TEXT("Emptying the stack"), Inventory->GetItemQuantity(TEXT("potion")), 0);

	// An empty stack keeps its binding: the bar reshuffling under the player's
	// thumb the moment they drink their last potion is worse than a greyed slot.
	Tick(Bar, 1.2f);
	TestFalse(TEXT("With none left, using fails"), Bar->UseSelected());
	TestEqual(TEXT("But the slot stays assigned"), Bar->GetSlot(0), FName(TEXT("potion")));
	TestNotNull(TEXT("And still resolves its item for display"), Bar->GetSelectedItem());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS
