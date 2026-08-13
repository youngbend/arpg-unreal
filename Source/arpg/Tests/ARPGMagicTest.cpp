// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ARPGCombatDummy.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGElementalCoating.h"
#include "ARPGGameplayTags.h"
#include "ARPGMagicCaster.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicComponent.h"
#include "ARPGMagicElement.h"
#include "ARPGMagicLoadout.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

/**
 * The magic system: combination resolution, selection, gating and cost.
 *
 * WHAT IS AND IS NOT COVERED. Everything here is the DECISION layer -- what a
 * set of readied elements resolves to, what it costs, what damage it would deal.
 * The discharge abilities that act on those decisions are not covered, because
 * they spawn effect actors and play montages that need authored content; the
 * damage formulas they consume are tested directly through
 * BuildDischargeContext instead.
 *
 * The one place that matters most is the power/damage split: they are two
 * independent curves, and the whole reason they are precomputed is so tuning one
 * cannot move the other. Several cases below exist purely to hold that line.
 */
namespace ARPGMagicTestUtils
{
	struct FTestWorld
	{
		UWorld* World = nullptr;

		FTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
		}

		~FTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};

	void Tick(UActorComponent* Component, float Seconds, float Step = 0.02f)
	{
		for (float Elapsed = 0.f; Elapsed < Seconds; Elapsed += Step)
		{
			Component->TickComponent(Step, LEVELTICK_All, nullptr);
		}
	}

	UARPGMagicElement* MakeElement(UObject* Outer, FGameplayTag Tag, float BaseDamage = 10.f)
	{
		UARPGMagicElement* Element = NewObject<UARPGMagicElement>(Outer);
		Element->ElementTag = Tag;
		Element->BaseDamage = BaseDamage;
		Element->ActivationCost = 0.f;
		Element->UsageRate = 1.f;
		return Element;
	}

	UARPGMagicCombinationEntry* MakeEntry(UObject* Outer, FGameplayTag A, FGameplayTag B,
		UARPGMagicElement* Result, int32 Scope = 0xF)
	{
		UARPGMagicCombinationEntry* Entry = NewObject<UARPGMagicCombinationEntry>(Outer);
		Entry->RequiredElements.AddTag(A);
		Entry->RequiredElements.AddTag(B);
		Entry->Result = Result;
		Entry->Scope = Scope;
		return Entry;
	}

	struct FRig
	{
		AARPGCombatDummy* Actor = nullptr;
		UARPGMagicComponent* Magic = nullptr;
		UARPGMagicLoadout* Loadout = nullptr;
		UARPGMagicCombinationTable* Table = nullptr;

		UARPGMagicElement* Fire = nullptr;
		UARPGMagicElement* Water = nullptr;
		UARPGMagicElement* Steam = nullptr;

		float Mana() const
		{
			return Actor->GetAbilitySystemComponent()->GetNumericAttribute(
				UARPGVitalSet::GetManaAttribute());
		}

		void SetMana(float Value) const
		{
			Actor->GetAbilitySystemComponent()->SetNumericAttributeBase(
				UARPGVitalSet::GetManaAttribute(), Value);
		}
	};

	/**
	 * The same rig on a caster, which supplies mastery levels. Only the gating
	 * and mastery cases need it -- everything else is deliberately built on a
	 * plain dummy, so those tests also prove the system works with NO progression
	 * wired up at all (the NPC case).
	 */
	FRig BuildCasterRig(UWorld* World, float FireLevel, float WaterLevel);

	/** Fire in the north slot, water in the west, and a fire+water->steam row. */
	FRig BuildRig(UWorld* World)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		FRig Rig;
		Rig.Actor = World->SpawnActor<AARPGCombatDummy>(
			AARPGCombatDummy::StaticClass(), FTransform::Identity, Params);

		UAbilitySystemComponent* ASC = Rig.Actor->GetAbilitySystemComponent();
		ASC->InitAbilityActorInfo(Rig.Actor, Rig.Actor);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxManaAttribute(), 100.f);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetManaAttribute(), 100.f);

		Rig.Loadout = NewObject<UARPGMagicLoadout>();
		Rig.Loadout->PageCount = 2;
		Rig.Loadout->MaxElements = 2;
		Rig.Loadout->ConformToPageCount();

		Rig.Fire = MakeElement(Rig.Loadout, TAG_Element_Fire, 20.f);
		Rig.Water = MakeElement(Rig.Loadout, TAG_Element_Water, 12.f);
		Rig.Steam = MakeElement(Rig.Loadout, TAG_Element_Steam, 40.f);

		Rig.Loadout->SetSlot(0, EARPGElementSlot::North, Rig.Fire);
		Rig.Loadout->SetSlot(0, EARPGElementSlot::West, Rig.Water);

		Rig.Table = NewObject<UARPGMagicCombinationTable>();
		Rig.Table->Entries.Add(MakeEntry(Rig.Table, TAG_Element_Fire, TAG_Element_Water, Rig.Steam));

		Rig.Magic = NewObject<UARPGMagicComponent>(Rig.Actor);
		Rig.Magic->Loadout = Rig.Loadout;
		Rig.Magic->CombinationTable = Rig.Table;
		Rig.Magic->RegisterComponent();

		return Rig;
	}

	FRig BuildCasterRig(UWorld* World, float FireLevel, float WaterLevel)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		FRig Rig;
		AARPGMagicCaster* Caster = World->SpawnActor<AARPGMagicCaster>(
			AARPGMagicCaster::StaticClass(), FTransform::Identity, Params);
		Rig.Actor = Caster;

		UAbilitySystemComponent* ASC = Caster->GetAbilitySystemComponent();
		ASC->InitAbilityActorInfo(Caster, Caster);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxManaAttribute(), 100.f);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetManaAttribute(), 100.f);

		Caster->ElementLevels.Add(TAG_Element_Fire, FireLevel);
		Caster->ElementLevels.Add(TAG_Element_Water, WaterLevel);

		Rig.Loadout = NewObject<UARPGMagicLoadout>();
		Rig.Loadout->PageCount = 1;
		Rig.Loadout->MaxElements = 2;
		Rig.Loadout->ConformToPageCount();

		Rig.Fire = MakeElement(Rig.Loadout, TAG_Element_Fire, 20.f);
		Rig.Water = MakeElement(Rig.Loadout, TAG_Element_Water, 12.f);
		Rig.Steam = MakeElement(Rig.Loadout, TAG_Element_Steam, 40.f);
		Rig.Steam->Complexity = 2;

		Rig.Loadout->SetSlot(0, EARPGElementSlot::North, Rig.Fire);
		Rig.Loadout->SetSlot(0, EARPGElementSlot::West, Rig.Water);

		Rig.Table = NewObject<UARPGMagicCombinationTable>();
		Rig.Table->Entries.Add(MakeEntry(Rig.Table, TAG_Element_Fire, TAG_Element_Water, Rig.Steam));

		Rig.Magic = Caster->Magic;
		Rig.Magic->Loadout = Rig.Loadout;
		Rig.Magic->CombinationTable = Rig.Table;

		return Rig;
	}
}

// ---------------------------------------------------------------------------
// Complexity gating
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicGateNoTrackerTest,
	"ARPG.Magic.Gating.NoProgressionMeansNoGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicGateNoTrackerTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World); // a plain dummy: no progression interface

	Rig.Fire->Complexity = 3;
	Rig.Water->Complexity = 3;

	Rig.Magic->ToggleElement(EARPGElementSlot::North);
	Rig.Magic->ToggleElement(EARPGElementSlot::West);

	// A caster with no progression is not a caster who has failed to train --
	// it is one the system does not apply to, which is exactly what an NPC is.
	// Gating off entirely is the deliberate behaviour, not a fallback.
	TestEqual(TEXT("Without progression, complexity does not gate"),
		Rig.Magic->GetActiveCount(), 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicGateBlocksTest,
	"ARPG.Magic.Gating.InsufficientMasteryBlocksTheSecondElement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicGateBlocksTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;

	// Trained in fire, a novice at water.
	FRig Rig = BuildCasterRig(Scope.World, /*FireLevel=*/4.f, /*WaterLevel=*/0.f);
	Rig.Fire->Complexity = 2;
	Rig.Water->Complexity = 2;

	Rig.Magic->ToggleElement(EARPGElementSlot::North);
	TestEqual(TEXT("A single element is never gated"), Rig.Magic->GetActiveCount(), 1);

	Rig.Magic->ToggleElement(EARPGElementSlot::West);

	// The gate runs BOTH ways. Level 4 in fire does not license holding water
	// alongside it: the caster must also be good enough at water to cover fire's
	// complexity. Checking one direction only would let a single mastered element
	// unlock every pairing.
	TestEqual(TEXT("Mastery in one element does not license the pair"),
		Rig.Magic->GetActiveCount(), 1);
	TestNull(TEXT("So no combination resolves"), Rig.Magic->GetResolvedCombination());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicGatePassesTest,
	"ARPG.Magic.Gating.SufficientMasteryAllowsTheCombination",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicGatePassesTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildCasterRig(Scope.World, /*FireLevel=*/2.f, /*WaterLevel=*/2.f);
	Rig.Fire->Complexity = 2;
	Rig.Water->Complexity = 2;

	Rig.Magic->ToggleElement(EARPGElementSlot::North);
	Rig.Magic->ToggleElement(EARPGElementSlot::West);

	// Level exactly equal to complexity passes -- the comparison is >=, so the
	// level a player is told they need is the level that works.
	TestEqual(TEXT("Equal mastery on both sides passes the gate"),
		Rig.Magic->GetActiveCount(), 2);
	TestSamePtr(TEXT("And the combination resolves"),
		Rig.Magic->GetResolvedCombination(), Rig.Steam);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicMasteryTest,
	"ARPG.Magic.Gating.MasteryScalesDamageButNotPoise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicMasteryTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildCasterRig(Scope.World, /*FireLevel=*/4.f, /*WaterLevel=*/4.f);

	AARPGMagicCaster* Caster = CastChecked<AARPGMagicCaster>(Rig.Actor);
	Caster->ElementDamageMultipliers.Add(TAG_Element_Fire, 2.f);

	Rig.Fire->BaseDamage = 20.f;
	Rig.Fire->BasePoiseDamage = 5.f;

	FARPGDischargeTypeSettings Burst;
	Burst.DamageMultiplier = 1.f;
	Burst.MaxChargeDamage = 0.f;
	Rig.Magic->DischargeSettings.Add(EARPGDischargeType::Burst, Burst);
	Rig.Magic->MasteryPowerBonus = 0.5f;

	Rig.Magic->ToggleElement(EARPGElementSlot::North);

	const FARPGDischargeContext Context = Rig.Magic->BuildDischargeContext(
		EARPGDischargeType::Burst, FVector::ZeroVector, FVector::ForwardVector, 0.f);

	TestEqual(TEXT("Mastery multiplies damage"), Context.ComputedDamage, 40.f);

	// Poise deliberately does NOT inherit mastery: it is a separate axis from
	// HP-damage power creep, so a master does not also stagger twice as hard.
	TestEqual(TEXT("But leaves poise alone"), Context.ComputedPoiseDamage, 5.f);

	// Power gets the full mastery bonus at level 4, on top of the window floor.
	TestEqual(TEXT("And raises the cosmetic power"), Context.PowerFraction, 1.5f);

	return true;
}

// ---------------------------------------------------------------------------
// Combination
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicCombinationTest,
	"ARPG.Magic.Combination.FireAndWaterMakeSteam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicCombinationTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Magic->ToggleElement(EARPGElementSlot::North);
	TestEqual(TEXT("One element readied"), Rig.Magic->GetActiveCount(), 1);

	// A single element is not a combination, even though a row mentions it.
	TestNull(TEXT("One element resolves to nothing"), Rig.Magic->GetResolvedCombination());
	TestSamePtr(TEXT("And displays as itself"), Rig.Magic->GetDisplayElement(), Rig.Fire);

	Rig.Magic->ToggleElement(EARPGElementSlot::West);
	TestSamePtr(TEXT("Fire plus water resolves to steam"),
		Rig.Magic->GetResolvedCombination(), Rig.Steam);
	TestSamePtr(TEXT("And steam is what drives the cast"),
		Rig.Magic->GetDisplayElement(), Rig.Steam);
	TestTrue(TEXT("So the spell can be cast"), Rig.Magic->CanDischarge());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicNoRecipeTest,
	"ARPG.Magic.Combination.UnmatchedPairCannotCast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicNoRecipeTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	// Replace water with an element that combines with nothing.
	UARPGMagicElement* Earth = MakeElement(Rig.Loadout, TAG_Element_Earth, 15.f);
	Rig.Loadout->SetSlot(0, EARPGElementSlot::West, Earth);

	Rig.Magic->ToggleElement(EARPGElementSlot::North);
	Rig.Magic->ToggleElement(EARPGElementSlot::West);

	TestEqual(TEXT("Both are readied"), Rig.Magic->GetActiveCount(), 2);
	TestNull(TEXT("But nothing resolves"), Rig.Magic->GetResolvedCombination());

	// Two elements with no recipe is a FAILED spell, not a two-element one. The
	// distinction matters: casting anyway would silently pick one of the two.
	TestFalse(TEXT("So it cannot be cast"), Rig.Magic->CanDischarge());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicScopeTest,
	"ARPG.Magic.Combination.ScopeNarrowsWhereARowApplies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicScopeTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UARPGMagicElement* Ice = MakeElement(Rig.Table, TAG_Element_Ice, 18.f);

	// The case the scope system exists for: air over water is a plausible ice
	// RECIPE in a caster's hands, and nonsense as world physics -- an air blast
	// over a lake is just wind.
	UARPGMagicCombinationEntry* HandOnly = MakeEntry(Rig.Table,
		TAG_Element_Air, TAG_Element_Water, Ice,
		static_cast<int32>(EARPGCombinationScope::Hand));
	Rig.Table->Entries.Add(HandOnly);

	FGameplayTagContainer AirWater;
	AirWater.AddTag(TAG_Element_Air);
	AirWater.AddTag(TAG_Element_Water);

	TestSamePtr(TEXT("In hand, air and water make ice"),
		Rig.Table->Resolve(AirWater, EARPGCombinationScope::Hand), Ice);
	TestNull(TEXT("In the world, they do not"),
		Rig.Table->Resolve(AirWater, EARPGCombinationScope::Collision));
	TestNull(TEXT("Nor on a fluid surface"),
		Rig.Table->Resolve(AirWater, EARPGCombinationScope::Surface));

	// And the all-scope steam row still answers everywhere.
	FGameplayTagContainer FireWater;
	FireWater.AddTag(TAG_Element_Fire);
	FireWater.AddTag(TAG_Element_Water);
	TestSamePtr(TEXT("An all-scope row answers in hand"),
		Rig.Table->Resolve(FireWater, EARPGCombinationScope::Hand), Rig.Steam);
	TestSamePtr(TEXT("And in the world"),
		Rig.Table->Resolve(FireWater, EARPGCombinationScope::Collision), Rig.Steam);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicExactMatchTest,
	"ARPG.Magic.Combination.MatchIsExactNotSubset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicExactMatchTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	FGameplayTagContainer Three;
	Three.AddTag(TAG_Element_Fire);
	Three.AddTag(TAG_Element_Water);
	Three.AddTag(TAG_Element_Air);

	// Containment rather than equality would resolve this as the two-way recipe
	// and make the third element silently vanish.
	TestNull(TEXT("A two-way row does not match a three-way mix"),
		Rig.Table->Resolve(Three, EARPGCombinationScope::Hand));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicPriorityTest,
	"ARPG.Magic.Combination.HighestPriorityWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicPriorityTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UARPGMagicElement* Lightning = MakeElement(Rig.Table, TAG_Element_Lightning, 30.f);
	UARPGMagicCombinationEntry* Better =
		MakeEntry(Rig.Table, TAG_Element_Fire, TAG_Element_Water, Lightning);
	Better->Priority = 10;
	Rig.Table->Entries.Add(Better);

	FGameplayTagContainer FireWater;
	FireWater.AddTag(TAG_Element_Fire);
	FireWater.AddTag(TAG_Element_Water);

	TestSamePtr(TEXT("The higher-priority row wins regardless of array order"),
		Rig.Table->Resolve(FireWater, EARPGCombinationScope::Hand), Lightning);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicAmplifyTest,
	"ARPG.Magic.Combination.ResultAsReactantIsAmplification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicAmplifyTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	// Fire + air -> fire is one element feeding the other, not a transmutation.
	// It needs no separate concept, which is the point being pinned here.
	UARPGMagicCombinationEntry* Fanned =
		MakeEntry(Rig.Table, TAG_Element_Fire, TAG_Element_Air, Rig.Fire);

	TestTrue(TEXT("A row whose result is a reactant amplifies"), Fanned->IsAmplifying());
	TestEqual(TEXT("And names which element grows"),
		Fanned->GetAmplifiedElement(), TAG_Element_Fire.GetTag());

	// Conduction also has result == reactant but means something else entirely,
	// so it must not fall through to amplification.
	Fanned->Mode = EARPGReactionMode::Conduct;
	TestFalse(TEXT("Conduction is not amplification"), Fanned->IsAmplifying());
	TestEqual(TEXT("It names what travels"),
		Fanned->GetConductedElement(), TAG_Element_Fire.GetTag());
	TestEqual(TEXT("And what carries it"),
		Fanned->GetConductorElement(), TAG_Element_Air.GetTag());

	// A plain combination amplifies nothing.
	TestFalse(TEXT("Steam is not an amplification"), Rig.Table->Entries[0]->IsAmplifying());

	return true;
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicSelectionTest,
	"ARPG.Magic.Selection.AdditiveAndCapped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicSelectionTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Fire->ActivationCost = 10.f;
	Rig.Water->ActivationCost = 10.f;

	Rig.Magic->ToggleElement(EARPGElementSlot::North);
	TestEqual(TEXT("Readying costs mana"), Rig.Mana(), 90.f);

	// Additive-only: pressing the same slot again is not a toggle-off.
	Rig.Magic->ToggleElement(EARPGElementSlot::North);
	TestEqual(TEXT("Re-pressing does not un-ready"), Rig.Magic->GetActiveCount(), 1);
	TestEqual(TEXT("Nor charge again"), Rig.Mana(), 90.f);

	Rig.Magic->ToggleElement(EARPGElementSlot::West);
	TestEqual(TEXT("A second element joins the mix"), Rig.Magic->GetActiveCount(), 2);

	// MaxElements is 2, so a third slot is refused even though it is filled.
	UARPGMagicElement* Earth = MakeElement(Rig.Loadout, TAG_Element_Earth);
	Earth->ActivationCost = 10.f;
	Rig.Loadout->SetSlot(0, EARPGElementSlot::South, Earth);

	const float BeforeThird = Rig.Mana();
	Rig.Magic->ToggleElement(EARPGElementSlot::South);
	TestEqual(TEXT("The cap holds"), Rig.Magic->GetActiveCount(), 2);
	TestEqual(TEXT("And a refused element costs nothing"), Rig.Mana(), BeforeThird);

	Rig.Magic->ClearSelection();
	TestEqual(TEXT("Clearing is wholesale"), Rig.Magic->GetActiveCount(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicUnaffordableTest,
	"ARPG.Magic.Selection.UnaffordableElementIsRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicUnaffordableTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Fire->ActivationCost = 40.f;
	Rig.SetMana(10.f);

	Rig.Magic->ToggleElement(EARPGElementSlot::North);

	TestEqual(TEXT("Nothing is readied"), Rig.Magic->GetActiveCount(), 0);
	TestEqual(TEXT("And nothing is spent"), Rig.Mana(), 10.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicPageTest,
	"ARPG.Magic.Selection.PageChangeClearsSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicPageTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UARPGMagicElement* Ice = MakeElement(Rig.Loadout, TAG_Element_Ice);
	Rig.Loadout->SetSlot(1, EARPGElementSlot::North, Ice);

	Rig.Magic->ToggleElement(EARPGElementSlot::North);
	TestSamePtr(TEXT("Setup: fire is readied on page 0"),
		Rig.Magic->GetDisplayElement(), Rig.Fire);

	// The mask indexes slots on the CURRENT page. Carrying it across would leave
	// the north bit set and silently re-point it at ice.
	Rig.Magic->SetCurrentPage(1);
	TestEqual(TEXT("Changing page clears the selection"), Rig.Magic->GetActiveCount(), 0);
	TestNull(TEXT("So nothing is readied"), Rig.Magic->GetDisplayElement());

	Rig.Magic->ToggleElement(EARPGElementSlot::North);
	TestSamePtr(TEXT("The new page's element is readied"),
		Rig.Magic->GetDisplayElement(), Ice);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicAutoReadyTest,
	"ARPG.Magic.Selection.AutoReadyRestoresTheLastCombination",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicAutoReadyTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Magic->SelectionInterval = 0.1f;
	Rig.Fire->ActivationCost = 5.f;
	Rig.Water->ActivationCost = 5.f;

	Rig.Magic->ToggleElement(EARPGElementSlot::North);
	Tick(Rig.Magic, 0.15f);
	Rig.Magic->ToggleElement(EARPGElementSlot::West);
	TestEqual(TEXT("Setup: steam is readied"), Rig.Magic->GetActiveCount(), 2);

	Rig.Magic->NotifyDischarged(Rig.Magic->BuildDischargeContext(
		EARPGDischargeType::Burst, FVector::ZeroVector, FVector::ForwardVector, 0.f));
	TestEqual(TEXT("Casting clears the mix"), Rig.Magic->GetActiveCount(), 0);

	const float BeforeAutoReady = Rig.Mana();
	Rig.Magic->BeginAutoReady();
	TestTrue(TEXT("Auto-ready is pending"), Rig.Magic->IsAutoReadyPending());

	// Long enough for both steps. Each pays the element's normal cost, so
	// repeating a combination is convenient but not free.
	Tick(Rig.Magic, 0.5f);

	TestEqual(TEXT("Both elements are back"), Rig.Magic->GetActiveCount(), 2);
	TestSamePtr(TEXT("And resolve to steam again"),
		Rig.Magic->GetResolvedCombination(), Rig.Steam);
	TestEqual(TEXT("Auto-ready charged for both"), Rig.Mana(), BeforeAutoReady - 10.f);
	TestFalse(TEXT("And is finished"), Rig.Magic->IsAutoReadyPending());

	return true;
}

// ---------------------------------------------------------------------------
// Cost and damage
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicUsageRateTest,
	"ARPG.Magic.Cost.CombinationReplacesPrimitiveRates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicUsageRateTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Fire->UsageRate = 1.f;
	Rig.Water->UsageRate = 2.f;
	Rig.Steam->UsageRate = 0.5f;

	Rig.Magic->ToggleElement(EARPGElementSlot::North);
	TestEqual(TEXT("One element uses its own rate"), Rig.Magic->GetUsageRate(), 1.f);

	Rig.Magic->ToggleElement(EARPGElementSlot::West);

	// REPLACES, not sums. A combo spell is priced on its own terms rather than
	// inheriting what its ingredients cost -- 0.5, not 3.
	TestEqual(TEXT("A combination replaces the sum with its own rate"),
		Rig.Magic->GetUsageRate(), 0.5f);

	FARPGDischargeTypeSettings Burst;
	Burst.MinManaCost = 10.f;
	Burst.MaxManaCost = 30.f;
	Rig.Magic->DischargeSettings.Add(EARPGDischargeType::Burst, Burst);

	TestEqual(TEXT("Zero charge costs the minimum, times the rate"),
		Rig.Magic->GetDischargeManaCost(EARPGDischargeType::Burst, 0.f), 5.f);
	TestEqual(TEXT("Full charge costs the maximum, times the rate"),
		Rig.Magic->GetDischargeManaCost(EARPGDischargeType::Burst, 1.f), 15.f);
	TestEqual(TEXT("And half charge interpolates"),
		Rig.Magic->GetDischargeManaCost(EARPGDischargeType::Burst, 0.5f), 10.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicDamageTest,
	"ARPG.Magic.Damage.ChargeAndTypeScaleIndependently",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicDamageTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Fire->BaseDamage = 20.f;
	Rig.Fire->BasePoiseDamage = 4.f;

	FARPGDischargeTypeSettings Burst;
	Burst.DamageMultiplier = 1.f;
	Burst.MaxChargeDamage = 0.5f; // full charge deals 50% more
	Rig.Magic->DischargeSettings.Add(EARPGDischargeType::Burst, Burst);

	FARPGDischargeTypeSettings Emanate;
	Emanate.DamageMultiplier = 0.5f;
	Emanate.MaxChargeDamage = 0.5f;
	Rig.Magic->DischargeSettings.Add(EARPGDischargeType::Emanate, Emanate);

	Rig.Magic->ToggleElement(EARPGElementSlot::North);

	const FARPGDischargeContext Tapped = Rig.Magic->BuildDischargeContext(
		EARPGDischargeType::Burst, FVector::ZeroVector, FVector::ForwardVector, 0.f);
	TestEqual(TEXT("No charge is the base damage"), Tapped.ComputedDamage, 20.f);

	const FARPGDischargeContext Full = Rig.Magic->BuildDischargeContext(
		EARPGDischargeType::Burst, FVector::ZeroVector, FVector::ForwardVector, 1.f);
	TestEqual(TEXT("Full charge adds the authored ratio"), Full.ComputedDamage, 30.f);

	// The per-type multiplier scales the whole curve, not just its top.
	const FARPGDischargeContext EmanateFull = Rig.Magic->BuildDischargeContext(
		EARPGDischargeType::Emanate, FVector::ZeroVector, FVector::ForwardVector, 1.f);
	TestEqual(TEXT("A weaker type scales down proportionally"),
		EmanateFull.ComputedDamage, 15.f);

	// Poise takes the same two STATIC knobs -- charge ratio and type multiplier.
	TestEqual(TEXT("Poise follows the charge ratio"), Full.ComputedPoiseDamage, 6.f);
	TestEqual(TEXT("And the type multiplier"), EmanateFull.ComputedPoiseDamage, 3.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicPowerTest,
	"ARPG.Magic.Damage.PowerIsIndependentOfDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicPowerTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Magic->MinPower = 1.f;
	Rig.Magic->MaxPower = 2.f;

	FARPGDischargeTypeSettings Burst;
	Burst.DamageMultiplier = 0.25f; // deliberately unlike the power window
	Burst.MaxChargeDamage = 3.f;
	Rig.Magic->DischargeSettings.Add(EARPGDischargeType::Burst, Burst);

	Rig.Magic->ToggleElement(EARPGElementSlot::North);

	const FARPGDischargeContext Tapped = Rig.Magic->BuildDischargeContext(
		EARPGDischargeType::Burst, FVector::ZeroVector, FVector::ForwardVector, 0.f);
	const FARPGDischargeContext Full = Rig.Magic->BuildDischargeContext(
		EARPGDischargeType::Burst, FVector::ZeroVector, FVector::ForwardVector, 1.f);

	// Power spans the authored window and is untouched by the type's damage
	// multiplier or charge ratio. This is the line the whole two-curve design
	// exists to hold: tuning damage must never move the visuals.
	TestEqual(TEXT("Power at no charge is the window's floor"), Tapped.PowerFraction, 1.f);
	TestEqual(TEXT("Power at full charge is its ceiling"), Full.PowerFraction, 2.f);
	TestEqual(TEXT("Normalised power is 0 at no charge"), Tapped.GetNormalisedPower(), 0.f);
	TestEqual(TEXT("And 1 at full"), Full.GetNormalisedPower(), 1.f);

	// Meanwhile damage moved by a completely different factor.
	TestEqual(TEXT("Damage followed its own curve, not power's"),
		Full.ComputedDamage / Tapped.ComputedDamage, 4.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicImbueTest,
	"ARPG.Magic.Consumption.ImbueIsFlatAndScalesWithMotion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicImbueTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Fire->BaseDamage = 20.f;
	Rig.Fire->BasePoiseDamage = 5.f;
	Rig.Magic->ImbueDamageMultiplier = 0.5f;
	Rig.Magic->ImbueManaCost = 15.f;

	Rig.Magic->ToggleElement(EARPGElementSlot::North);

	// Imbue has no charge term at all -- the swing's motion value is what scales
	// it, which belongs to the attack rather than the spell.
	TestEqual(TEXT("Imbue damage scales with motion value"),
		Rig.Magic->GetImbueDamage(Rig.Fire, 2.f), 20.f);
	TestEqual(TEXT("Poise likewise"),
		Rig.Magic->GetImbuePoiseDamage(Rig.Fire, 2.f), 5.f);

	// The coating is the whole payload the swing receives, and it is what
	// configures the elemental hitbox -- the two formulas above are only part of
	// it. Checked here rather than through the ability for the reason the
	// discharge formulas are: an ability needs an avatar, an ASC and a granted
	// spec before it runs.
	UARPGDamageTypeAsset* FireDamage = NewObject<UARPGDamageTypeAsset>();
	Rig.Fire->DamageType = FireDamage;
	Rig.Fire->StatusDuration = 4.f;
	Rig.Fire->ImbueReachScale = 2.5f;

	const FARPGElementalCoating Coating = Rig.Magic->BuildImbueCoating(Rig.Fire, 2.f);

	TestEqual(TEXT("The coating carries the imbue damage"), Coating.BaseDamage, 20.f);
	TestEqual(TEXT("And the imbue poise"), Coating.PoiseDamage, 5.f);
	TestSamePtr(TEXT("And the element's own damage type, which is the whole point"),
		Coating.DamageType.Get(), FireDamage);
	TestTrue(TEXT("Stamped with the element that delivered it"),
		Coating.MagicElementTag == TAG_Element_Fire);

	// Reach travels with the element, not the weapon: this is what makes the
	// coating a hitbox of its own rather than a second payload on the blade's.
	TestEqual(TEXT("And the element's reach"), Coating.TraceRadiusScale, 2.5f);
	TestFalse(TEXT("A coating with damage in it is not empty"), Coating.IsEmpty());

	// Nothing readied means no coating, which is what the imbue checks before
	// standing up an elemental hitbox at all.
	TestTrue(TEXT("No element, no coating"), Rig.Magic->BuildImbueCoating(nullptr, 2.f).IsEmpty());

	const float Before = Rig.Mana();
	UARPGMagicElement* Consumed = Rig.Magic->ConsumeForImbue();

	TestSamePtr(TEXT("Consuming returns the readied element"), Consumed, Rig.Fire);
	TestEqual(TEXT("At a flat cost"), Rig.Mana(), Before - 15.f);
	TestEqual(TEXT("And clears the mix"), Rig.Magic->GetActiveCount(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGMagicConsumeFailsTest,
	"ARPG.Magic.Consumption.UnaffordableConsumptionKeepsTheElement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGMagicConsumeFailsTest::RunTest(const FString& Parameters)
{
	using namespace ARPGMagicTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Magic->DodgeManaCost = 50.f;
	Rig.Magic->ToggleElement(EARPGElementSlot::North);
	Rig.SetMana(10.f);

	UARPGMagicElement* Consumed = Rig.Magic->ConsumeForDodge();

	// A failed consumption must not eat the element: the player already paid to
	// ready it, and silently losing it to a failed dodge is the worst outcome.
	TestNull(TEXT("Nothing is consumed"), Consumed);
	TestEqual(TEXT("The element stays readied"), Rig.Magic->GetActiveCount(), 1);
	TestEqual(TEXT("And nothing is spent"), Rig.Mana(), 10.f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
