// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ARPGGameplayTags.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicElement.h"
#include "ARPGSpreadDefinition.h"
#include "ARPGSpreadFuelMap.h"
#include "ARPGSpreadSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

/**
 * The fire spread field.
 *
 * THE PROPERTY WORTH PROTECTING is that reach is BOUNDED BY ENERGY. Everything
 * else about the simulation is tuning, but "a fireball lights a patch and the
 * patch burns out" versus "a fireball consumes the map" is the difference
 * between a mechanic and a bug -- and it is a single tuning slip away, because
 * the sub-critical condition (a cell yields less over its life than lighting the
 * next one costs) is a relationship between three authored numbers rather than
 * anything the code enforces.
 *
 * Every case below drives StepSimulation directly at a fixed step, so results
 * are deterministic rather than depending on frame timing.
 */
namespace ARPGSpreadTestUtils
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

	UARPGMagicElement* MakeElement(UObject* Outer, FGameplayTag Tag)
	{
		UARPGMagicElement* Element = NewObject<UARPGMagicElement>(Outer);
		Element->ElementTag = Tag;
		return Element;
	}

	/** A sub-critical fire: a cell yields less over its life than ignition costs. */
	UARPGSpreadDefinition* MakeFire(UObject* Outer, UARPGMagicElement* Element)
	{
		UARPGSpreadDefinition* Definition = NewObject<UARPGSpreadDefinition>(Outer);
		Definition->Element = Element;
		Definition->IgnitionThreshold = 1.f;
		Definition->SpreadRate = 2.f;
		Definition->DecayRate = 0.5f;
		Definition->WindBias = 0.f;
		Definition->bConserveEnergy = true;
		Definition->FuelEnergyYield = 0.05f; // 0.05 * 4s = 0.2, well under 1.0
		Definition->FuelSeconds = 4.f;
		Definition->TransferKeep = 0.5f;
		Definition->bUsesField = true;
		Definition->ContactDamagePerSecond = 0.f;
		return Definition;
	}

	/** Configures the subsystem with one medium and a small, fast grid. */
	UARPGSpreadSubsystem* Setup(UWorld* World, UARPGSpreadDefinition* Definition)
	{
		UARPGSpreadSubsystem* Spread = World->GetSubsystem<UARPGSpreadSubsystem>();
		if (!Spread)
		{
			return nullptr;
		}

		// A small chunk with coarse cells, so a handful of steps covers a
		// meaningful distance and a test runs in milliseconds.
		Spread->ChunkSize = 1600.f;
		Spread->FieldResolution = 16; // 100cm cells
		Spread->WindStrength = 0.f;
		Spread->Definitions = { Definition };

		return Spread;
	}

	void Run(UARPGSpreadSubsystem* Spread, int32 Steps, float Step = 0.1f)
	{
		for (int32 Index = 0; Index < Steps; ++Index)
		{
			Spread->StepSimulation(Step);
		}
	}
}

// ---------------------------------------------------------------------------
// Energy
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSpreadSeedTest,
	"ARPG.World.Spread.SeedingLightsTheGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSpreadSeedTest::RunTest(const FString& Parameters)
{
	using namespace ARPGSpreadTestUtils;
	FTestWorld Scope;

	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);
	UARPGSpreadSubsystem* Spread = Setup(Scope.World, MakeFire(GetTransientPackage(), Fire));

	if (!Spread)
	{
		AddError(TEXT("Setup: no spread subsystem in the test world."));
		return false;
	}

	const FVector Origin(800, 800, 0);

	TestFalse(TEXT("Nothing burns to begin with"), Spread->IsBurning(Origin, TAG_Element_Fire));

	const float Deposited = Spread->AddExposure(Origin, 150.f, 4.f, TAG_Element_Fire);

	TestTrue(TEXT("Seeding deposits energy"), Deposited > 0.f);
	TestTrue(TEXT("And lights the ground"), Spread->IsBurning(Origin, TAG_Element_Fire));

	// A direct seed banks in FULL -- transfer keep governs only fire-to-fire
	// hand-off, not what a spell puts in.
	TestTrue(TEXT("The seed lands in the cell's bank"),
		Spread->GetFieldEnergy(Origin, TAG_Element_Fire) > 0.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSpreadReachTest,
	"ARPG.World.Spread.ReachScalesWithEnergyPutIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSpreadReachTest::RunTest(const FString& Parameters)
{
	using namespace ARPGSpreadTestUtils;

	// THE CENTRAL PROPERTY. Under a conserved budget, how far a fire travels is a
	// LINEAR function of how much energy went in -- not a binary stall-or-
	// firestorm. Two otherwise identical fires, one seeded ten times harder.
	auto BurntCells = [this](float SeedAmount) -> int32
	{
		FTestWorld Scope;
		UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);
		UARPGSpreadSubsystem* Spread = Setup(Scope.World, MakeFire(GetTransientPackage(), Fire));

		Spread->AddExposure(FVector(800, 800, 0), 100.f, SeedAmount, TAG_Element_Fire);
		Run(Spread, 60);

		// Residue is monotonic, so it records everywhere the fire REACHED rather
		// than where it happens to be at the end of the run.
		int32 Count = 0;
		for (int32 Y = 0; Y < 16; ++Y)
		{
			for (int32 X = 0; X < 16; ++X)
			{
				const FVector Cell(X * 100.f + 50.f, Y * 100.f + 50.f, 0.f);
				if (Spread->GetFieldResidue(Cell, TAG_Element_Fire) > 0.01f)
				{
					++Count;
				}
			}
		}
		return Count;
	};

	const int32 Small = BurntCells(2.f);
	const int32 Large = BurntCells(40.f);

	TestTrue(TEXT("A small seed burns something"), Small > 0);
	TestTrue(TEXT("A large seed burns strictly more"), Large > Small);

	// And crucially it does NOT consume everything: sub-critical fuel means the
	// front dies at a range set by what started it.
	TestTrue(TEXT("But even a large seed burns out rather than taking the map"),
		Large < 16 * 16);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSpreadBurnoutTest,
	"ARPG.World.Spread.SubCriticalFuelAlwaysBurnsOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSpreadBurnoutTest::RunTest(const FString& Parameters)
{
	using namespace ARPGSpreadTestUtils;
	FTestWorld Scope;

	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);
	UARPGSpreadSubsystem* Spread = Setup(Scope.World, MakeFire(GetTransientPackage(), Fire));

	Spread->AddExposure(FVector(800, 800, 0), 150.f, 10.f, TAG_Element_Fire);

	Run(Spread, 10);
	TestTrue(TEXT("Setup: it is alight"), Spread->GetBurningCellCount(TAG_Element_Fire) > 0);

	// Long enough for every cell to exhaust its fuel several times over.
	Run(Spread, 400);

	TestEqual(TEXT("Eventually nothing is burning at all"),
		Spread->GetBurningCellCount(TAG_Element_Fire), 0);

	// And the ground remembers, which is what the char shader reads.
	TestTrue(TEXT("But the burnt ground is marked"),
		Spread->GetFieldResidue(FVector(800, 800, 0), TAG_Element_Fire) > 0.5f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSpreadFirebreakTest,
	"ARPG.World.Spread.GroundWithNoFuelCannotBurn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSpreadFirebreakTest::RunTest(const FString& Parameters)
{
	using namespace ARPGSpreadTestUtils;
	FTestWorld Scope;

	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);
	UARPGSpreadSubsystem* Spread = Setup(Scope.World, MakeFire(GetTransientPackage(), Fire));

	const FVector Bare(800, 800, 0);

	// A river bed, a road, bare rock. Stripping the fuel first.
	Spread->SetFieldFuel(Bare, 150.f, 0.f, TAG_Element_Fire);

	const float Deposited = Spread->AddExposure(Bare, 100.f, 20.f, TAG_Element_Fire);

	// Refused outright rather than accumulated. Letting it accumulate is what
	// made a firebreak READ as fully alight and deal contact damage to anyone
	// standing on it, while still never spreading onward.
	TestEqual(TEXT("Fuelless ground accepts no exposure at all"), Deposited, 0.f);
	TestFalse(TEXT("So it does not burn"), Spread->IsBurning(Bare, TAG_Element_Fire));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSpreadFuelMapTest,
	"ARPG.World.Spread.FuelMapGatesWhereFireTakesHold",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSpreadFuelMapTest::RunTest(const FString& Parameters)
{
	using namespace ARPGSpreadTestUtils;
	FTestWorld Scope;

	UARPGSpreadFuelMap* Map = NewObject<UARPGSpreadFuelMap>();
	Map->Resolution = 16;
	Map->ChunkSize = 1600.f;

	// An unbaked chunk burns. That is deliberate: a missing entry means "never
	// part of the baked world", not "no fuel", and conflating the two would make
	// an unbaked test level silently fireproof.
	TestEqual(TEXT("An unbaked chunk reads as full fuel"),
		Map->GetFuelAt(FIntPoint(5, 5), 0.5f, 0.5f), 1.f);

	// Half the chunk bare, half full.
	TArray<uint8> Cells;
	Cells.SetNumZeroed(16 * 16);
	for (int32 Y = 8; Y < 16; ++Y)
	{
		for (int32 X = 0; X < 16; ++X)
		{
			Cells[Y * 16 + X] = 255;
		}
	}
	Map->SetChunkCells(FIntPoint(0, 0), Cells);

	TestEqual(TEXT("The bare half reads as no fuel"), Map->GetFuelAt(FIntPoint(0, 0), 0.5f, 0.2f), 0.f);
	TestEqual(TEXT("And the grassy half as full"), Map->GetFuelAt(FIntPoint(0, 0), 0.5f, 0.8f), 1.f);

	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);
	UARPGSpreadSubsystem* Spread = Setup(Scope.World, MakeFire(GetTransientPackage(), Fire));
	Spread->FuelMap = Map;

	// The bake reaches the simulation: seeding the bare half does nothing, and
	// the same seed on the grassy half lights.
	TestEqual(TEXT("Seeding baked-bare ground deposits nothing"),
		Spread->AddExposure(FVector(800, 300, 0), 50.f, 10.f, TAG_Element_Fire), 0.f);

	TestTrue(TEXT("Seeding baked grass does"),
		Spread->AddExposure(FVector(800, 1300, 0), 50.f, 10.f, TAG_Element_Fire) > 0.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSpreadWindTest,
	"ARPG.World.Spread.WindSkewsPropagationDownwind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSpreadWindTest::RunTest(const FString& Parameters)
{
	using namespace ARPGSpreadTestUtils;
	FTestWorld Scope;

	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);
	UARPGSpreadDefinition* Definition = MakeFire(GetTransientPackage(), Fire);
	Definition->WindBias = 1.f;

	UARPGSpreadSubsystem* Spread = Setup(Scope.World, Definition);
	Spread->WindDirection = FVector2D(1.f, 0.f);
	Spread->WindStrength = 1.f;

	const FVector Origin(800, 800, 0);
	Spread->AddExposure(Origin, 100.f, 30.f, TAG_Element_Fire);
	Run(Spread, 40);

	// Residue rather than intensity: it records where the fire REACHED, not
	// where it is at the moment the run happens to stop.
	float Downwind = 0.f;
	float Upwind = 0.f;
	for (int32 Step = 1; Step <= 5; ++Step)
	{
		Downwind += Spread->GetFieldResidue(Origin + FVector(Step * 100.f, 0, 0), TAG_Element_Fire);
		Upwind += Spread->GetFieldResidue(Origin - FVector(Step * 100.f, 0, 0), TAG_Element_Fire);
	}

	TestTrue(TEXT("A full-bias fire in a gale runs downwind"), Downwind > Upwind);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSpreadRainTest,
	"ARPG.World.Spread.RainQuenchesFire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSpreadRainTest::RunTest(const FString& Parameters)
{
	using namespace ARPGSpreadTestUtils;
	FTestWorld Scope;

	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);
	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);

	UARPGSpreadDefinition* FireDefinition = MakeFire(GetTransientPackage(), Fire);

	UARPGSpreadDefinition* WaterDefinition = NewObject<UARPGSpreadDefinition>();
	WaterDefinition->Element = Water;
	WaterDefinition->IgnitionThreshold = 0.5f;
	WaterDefinition->SpreadRate = 0.f;   // rain falls, it does not creep
	WaterDefinition->DecayRate = 0.f;
	WaterDefinition->bConserveEnergy = false;
	WaterDefinition->bPermanent = true;  // and does not consume fuel
	WaterDefinition->bUsesField = true;
	WaterDefinition->ContactDamagePerSecond = 0.f;

	// ONE AUTHORED ROW, TWO SOLVERS. The same rates that make a water jet punch
	// through a fireball make rain quench a grass fire -- this is the field-scope
	// reading of the relationship, not a second rule set.
	UARPGMagicCombinationTable* Table = NewObject<UARPGMagicCombinationTable>();
	UARPGMagicCombinationEntry* Entry = NewObject<UARPGMagicCombinationEntry>(Table);
	Entry->RequiredElements.AddTag(TAG_Element_Fire);
	Entry->RequiredElements.AddTag(TAG_Element_Water);
	Entry->Result = Water;
	Entry->Scope = static_cast<int32>(EARPGCombinationScope::Field);
	Entry->ConsumptionRates.Add(TAG_Element_Fire, 4.f);  // fire dies fast
	Entry->ConsumptionRates.Add(TAG_Element_Water, 0.f); // rain does not
	Table->Entries.Add(Entry);

	UARPGSpreadSubsystem* Spread = Scope.World->GetSubsystem<UARPGSpreadSubsystem>();
	Spread->ChunkSize = 1600.f;
	Spread->FieldResolution = 16;
	Spread->WindStrength = 0.f;
	Spread->Definitions = { FireDefinition, WaterDefinition };
	Spread->CombinationTable = Table;

	const FVector Origin(800, 800, 0);

	Spread->AddExposure(Origin, 150.f, 20.f, TAG_Element_Fire);
	Run(Spread, 10);

	const int32 Burning = Spread->GetBurningCellCount(TAG_Element_Fire);
	TestTrue(TEXT("Setup: a fire is running"), Burning > 0);

	// Now it rains.
	Spread->AddExposure(Origin, 600.f, 5.f, TAG_Element_Water);
	Run(Spread, 20);

	TestEqual(TEXT("Rain puts the fire out"),
		Spread->GetBurningCellCount(TAG_Element_Fire), 0);

	// And takes its bank with it, so the moment the rain stops the fire does not
	// re-ignite at the reach it had before.
	TestEqual(TEXT("Leaving no banked energy to restart from"),
		Spread->GetFieldEnergy(Origin, TAG_Element_Fire), 0.f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
