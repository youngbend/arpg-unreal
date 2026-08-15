// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ARPGGameplayTags.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicElement.h"
#include "ARPGSpreadDefinition.h"
#include "ARPGSpreadFuelMap.h"
#include "ARPGFuelComponent.h"
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

	// A CELL NAMES A SURFACE NOW, not an amount. An unbaked chunk still burns: a
	// missing entry means "never part of the baked world", not "no fuel", and
	// conflating the two would make an unbaked test level silently fireproof.
	// Default is what a definition's table falls back to.
	TestEqual(TEXT("An unbaked chunk reads as ordinary ground"),
		static_cast<int32>(Map->GetSurfaceAt(FIntPoint(5, 5), 0.5f, 0.5f)),
		static_cast<int32>(SurfaceType_Default));

	// Half the chunk stone, half grass.
	TArray<uint8> Cells;
	Cells.Init(static_cast<uint8>(SurfaceType2), 16 * 16);   // stone
	for (int32 Y = 8; Y < 16; ++Y)
	{
		for (int32 X = 0; X < 16; ++X)
		{
			Cells[Y * 16 + X] = static_cast<uint8>(SurfaceType1);   // grass
		}
	}
	Map->SetChunkCells(FIntPoint(0, 0), Cells);

	TestEqual(TEXT("The bare half is stone"),
		static_cast<int32>(Map->GetSurfaceAt(FIntPoint(0, 0), 0.5f, 0.2f)),
		static_cast<int32>(SurfaceType2));
	TestEqual(TEXT("And the grassy half is grass"),
		static_cast<int32>(Map->GetSurfaceAt(FIntPoint(0, 0), 0.5f, 0.8f)),
		static_cast<int32>(SurfaceType1));

	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);
	UARPGSpreadDefinition* Burns = MakeFire(GetTransientPackage(), Fire);

	// AND WHAT THAT IS WORTH IS THE MEDIUM'S BUSINESS. Grass carries this fire,
	// stone is a firebreak, and the map said neither of those things.
	Burns->SurfaceFuel.Add(SurfaceType1, 1.f);
	Burns->SurfaceFuel.Add(SurfaceType2, 0.f);
	Burns->DefaultSurfaceFuel = 1.f;

	// No jitter, so this test asserts on the firebreak rather than on noise.
	Map->JitterRange = 0.f;

	UARPGSpreadSubsystem* Spread = Setup(Scope.World, Burns);
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


// ---------------------------------------------------------------------------
// Objects that are fuel
//
// The ground burns and objects did not: fuel is baked per cell, so a wooden
// crate on bare stone was scenery the fire went round. A fuel component is the
// other half -- something that feeds the fire where it stands, has a finite
// amount of itself to give, and can be caught halfway through giving it.
// ---------------------------------------------------------------------------

namespace ARPGSpreadTestUtils
{
	/** A crate: an actor with a footprint and something to burn. */
	inline UARPGFuelComponent* MakeCrate(UWorld* World, const FVector& At, float Seconds)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(At), Params);

		UARPGFuelComponent* Fuel = NewObject<UARPGFuelComponent>(Actor);
		Fuel->MediumTag = TAG_Element_Fire;
		Fuel->FuelSeconds = Seconds;
		Fuel->Radius = 120.f;
		Fuel->Output = 2.f;
		Fuel->CatchThreshold = 0.15f;
		Fuel->CharParameterIndex = -1;   // no primitives to publish onto
		Fuel->RegisterComponent();

		return Fuel;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFuelBurnsTest,
	"ARPG.World.Spread.Fuel.AnObjectInAFireIsConsumedByIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFuelBurnsTest::RunTest(const FString& Parameters)
{
	using namespace ARPGSpreadTestUtils;
	FTestWorld Scope;

	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);
	UARPGSpreadSubsystem* Spread = Setup(Scope.World, MakeFire(GetTransientPackage(), Fire));

	const FVector Origin(0, 0, 0);
	UARPGFuelComponent* Crate = MakeCrate(Scope.World, Origin, /*Seconds=*/2.f);

	TestEqual(TEXT("A fresh crate is whole"), Crate->FuelRemaining, 2.f);
	TestEqual(TEXT("And uncharred"), Crate->GetCharred(), 0.f);
	TestFalse(TEXT("And not alight"), Crate->IsAlight());

	// NOT BURNING UNTIL THERE IS A FIRE. A crate is not slowly rotting.
	Run(Spread, 5);
	TestEqual(TEXT("Nothing burns it on its own"), Crate->FuelRemaining, 2.f);

	Spread->AddExposure(Origin, 150.f, 4.f, TAG_Element_Fire);
	TestTrue(TEXT("Setup: the ground is alight"), Spread->IsBurning(Origin, TAG_Element_Fire));

	Run(Spread, 5);

	TestTrue(TEXT("Standing in fire, it catches"), Crate->IsAlight());
	TestTrue(TEXT("And is being consumed"), Crate->FuelRemaining < 2.f);
	TestTrue(TEXT("Which shows as char"), Crate->GetCharred() > 0.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFuelHalfBurntTest,
	"ARPG.World.Spread.Fuel.QuenchedHalfwayLeavesAHalfBurntCrate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFuelHalfBurntTest::RunTest(const FString& Parameters)
{
	using namespace ARPGSpreadTestUtils;
	FTestWorld Scope;

	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);
	UARPGSpreadSubsystem* Spread = Setup(Scope.World, MakeFire(GetTransientPackage(), Fire));

	const FVector Origin(0, 0, 0);
	UARPGFuelComponent* Crate = MakeCrate(Scope.World, Origin, /*Seconds=*/10.f);

	Spread->AddExposure(Origin, 150.f, 6.f, TAG_Element_Fire);
	Run(Spread, 10);

	const float PartWay = Crate->FuelRemaining;

	TestTrue(TEXT("It has burned some"), PartWay < 10.f);
	TestTrue(TEXT("But not all"), PartWay > 0.f);

	// PUT IT OUT. Water on the field, which is the ordinary extinguish path --
	// nothing here knows the fire went out for a reason rather than by running
	// down.
	Spread->Extinguish(Origin, 400.f, TAG_Element_Fire);
	Run(Spread, 20);

	TestFalse(TEXT("The fire is out"), Spread->IsBurning(Origin, TAG_Element_Fire));
	TestFalse(TEXT("So the crate stops burning"), Crate->IsAlight());

	// AND STOPS WHERE IT STOPPED. Nothing restores fuel, resets a timer or decays
	// anything: the burn is spent only while the cells are alight, and a half
	// burnt crate is what that means rather than something implemented.
	TestEqual(TEXT("Half burnt, and staying that way"),
		Crate->FuelRemaining, PartWay, 0.001f);

	const float Charred = Crate->GetCharred();
	TestTrue(TEXT("Visibly so"), Charred > 0.f && Charred < 1.f);

	// AND IT IS STILL FUEL. Relight it and it burns the remainder -- which also
	// falls out rather than being written.
	Spread->AddExposure(Origin, 150.f, 6.f, TAG_Element_Fire);
	Run(Spread, 10);

	TestTrue(TEXT("Relit, it carries on from where it was"),
		Crate->FuelRemaining < PartWay);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFuelSpentTest,
	"ARPG.World.Spread.Fuel.BurningOutSaysSoWithoutDecidingWhatItMeans",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFuelSpentTest::RunTest(const FString& Parameters)
{
	using namespace ARPGSpreadTestUtils;
	FTestWorld Scope;

	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);
	UARPGSpreadSubsystem* Spread = Setup(Scope.World, MakeFire(GetTransientPackage(), Fire));

	const FVector Origin(0, 0, 0);
	UARPGFuelComponent* Crate = MakeCrate(Scope.World, Origin, /*Seconds=*/0.3f);

	Spread->AddExposure(Origin, 150.f, 8.f, TAG_Element_Fire);
	Run(Spread, 20);

	TestEqual(TEXT("It burned out"), Crate->FuelRemaining, 0.f);
	TestTrue(TEXT("Which is spent"), Crate->IsSpent());
	TestEqual(TEXT("And fully charred"), Crate->GetCharred(), 1.f);
	TestFalse(TEXT("And no longer alight"), Crate->IsAlight());

	// SPENT IS NOT DESTROYED. A log becomes charcoal, a rope parts, a barricade
	// collapses -- the object decides, and nothing here decided for it.
	TestNotNull(TEXT("The actor is still there"), Crate->GetOwner());

	// AND IT CAN BE PUT BACK. A repaired barricade burns again without anyone
	// re-registering it, which is why spent sources stay on the register.
	Crate->Replenish(0.3f);
	TestEqual(TEXT("Repaired, it is whole again"), Crate->GetCharred(), 0.f);

	Spread->AddExposure(Origin, 150.f, 8.f, TAG_Element_Fire);
	Run(Spread, 5);

	TestTrue(TEXT("And burns again"), Crate->FuelRemaining < 0.3f);

	return true;
}

// ---------------------------------------------------------------------------
// What kind of ground it is
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSurfaceFuelTest,
	"ARPG.World.Spread.Fuel.TheGroundSaysWhatItIsAndTheMediumSaysWhatThatIsWorth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSurfaceFuelTest::RunTest(const FString& Parameters)
{
	using namespace ARPGSpreadTestUtils;

	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);
	UARPGSpreadDefinition* Definition = MakeFire(GetTransientPackage(), Fire);

	// A TABLE WITH ONLY THE INTERESTING CASES. Grass burns, stone does not, and
	// everything else is ordinary -- a medium should not have to enumerate every
	// surface in the project to say that it burns grass.
	Definition->SurfaceFuel.Add(SurfaceType1, 1.f);   // grass
	Definition->SurfaceFuel.Add(SurfaceType2, 0.f);   // stone
	Definition->DefaultSurfaceFuel = 0.5f;

	TestEqual(TEXT("Grass carries it"), Definition->GetSurfaceFuel(SurfaceType1), 1.f);
	TestEqual(TEXT("Stone is a firebreak"), Definition->GetSurfaceFuel(SurfaceType2), 0.f);

	// THE FALLBACK IS GENEROUS ON PURPOSE. A missing bake, a surface nobody has
	// added to the table, and a test world all land here, and "burns when it
	// should not" is far easier to notice than "silently fireproof".
	TestEqual(TEXT("Anything unlisted is ordinary"),
		Definition->GetSurfaceFuel(SurfaceType7), 0.5f);

	// AND THE SAME GROUND IS WORTH SOMETHING ELSE TO SOMETHING ELSE, which one
	// element-agnostic amount per cell could never say. A marsh refuses fire and
	// carries a frost.
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);
	UARPGSpreadDefinition* Frost = MakeFire(GetTransientPackage(), Ice);
	Frost->SurfaceFuel.Add(SurfaceType1, 0.f);
	Frost->SurfaceFuel.Add(SurfaceType3, 1.f);   // marsh
	Frost->DefaultSurfaceFuel = 0.f;

	TestEqual(TEXT("A frost does not cross dry grass"),
		Frost->GetSurfaceFuel(SurfaceType1), 0.f);
	TestEqual(TEXT("But crosses the marsh fire would not"),
		Frost->GetSurfaceFuel(SurfaceType3), 1.f);
	TestEqual(TEXT("Where fire finds the marsh ordinary"),
		Definition->GetSurfaceFuel(SurfaceType3), 0.5f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFuelJitterTest,
	"ARPG.World.Spread.Fuel.NoTwoCellsHoldExactlyTheSameAndTheSameCellAlwaysDoes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFuelJitterTest::RunTest(const FString& Parameters)
{
	UARPGSpreadFuelMap* Map = NewObject<UARPGSpreadFuelMap>();
	Map->Resolution = 16;
	Map->JitterRange = 0.35f;

	TArray<uint8> Cells;
	Cells.Init(static_cast<uint8>(SurfaceType1), 16 * 16);
	Map->SetChunkCells(FIntPoint(0, 0), Cells);

	// A UNIFORM GRID BURNS IN DIAMONDS. Every cell in a ring reaches ignition on
	// the same tick, so the front is a shape the grid chose rather than one the
	// fire did -- and that regularity is what made the same fire look different
	// depending on where the viewer stood relative to a cell boundary.
	TSet<float> Seen;
	float Lowest = 2.f;
	float Highest = 0.f;

	for (int32 Y = 0; Y < 16; ++Y)
	{
		for (int32 X = 0; X < 16; ++X)
		{
			const float Jitter = Map->GetJitterAt(FIntPoint(0, 0),
				(X + 0.5f) / 16.f, (Y + 0.5f) / 16.f);

			Seen.Add(Jitter);
			Lowest = FMath::Min(Lowest, Jitter);
			Highest = FMath::Max(Highest, Jitter);
		}
	}

	TestTrue(TEXT("Cells vary rather than agreeing"), Seen.Num() > 32);
	TestTrue(TEXT("Within the range they were given"), Lowest >= 0.65f - 0.01f);
	TestTrue(TEXT("At both ends of it"), Highest <= 1.35f + 0.01f);
	TestTrue(TEXT("And actually spread across it"), Highest - Lowest > 0.3f);

	// RAGGED, NOT RANDOM. Hashed from the cell's coordinate, so a patch of ground
	// always burns the same way and two bakes of one level agree.
	UARPGSpreadFuelMap* Again = NewObject<UARPGSpreadFuelMap>();
	Again->Resolution = 16;
	Again->JitterRange = 0.35f;
	Again->SetChunkCells(FIntPoint(0, 0), Cells);

	TestEqual(TEXT("The same cell is the same every bake"),
		Again->GetJitterAt(FIntPoint(0, 0), 0.3f, 0.7f),
		Map->GetJitterAt(FIntPoint(0, 0), 0.3f, 0.7f));

	// AND A DIFFERENT CHUNK IS DIFFERENT GROUND, so the pattern does not tile.
	TestNotEqual(TEXT("A different chunk is not the same ground"),
		Map->GetJitterAt(FIntPoint(0, 0), 0.3f, 0.7f),
		Map->GetJitterAt(FIntPoint(1, 0), 0.3f, 0.7f));

	// An unbaked map is exactly 1, so nothing that never asked for noise gets it.
	UARPGSpreadFuelMap* Bare = NewObject<UARPGSpreadFuelMap>();
	TestEqual(TEXT("Unbaked ground has no variation"),
		Bare->GetJitterAt(FIntPoint(0, 0), 0.5f, 0.5f), 1.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGScorchTest,
	"ARPG.World.Spread.Fuel.GroundBurnsPartwayAndStaysThatWay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGScorchTest::RunTest(const FString& Parameters)
{
	using namespace ARPGSpreadTestUtils;
	FTestWorld Scope;

	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);
	UARPGSpreadDefinition* Definition = MakeFire(GetTransientPackage(), Fire);
	Definition->FuelSeconds = 4.f;

	UARPGSpreadSubsystem* Spread = Setup(Scope.World, Definition);

	const FVector Origin(0, 0, 0);

	TestEqual(TEXT("Unburnt ground is unscorched"),
		Spread->GetScorchAt(Origin, TAG_Element_Fire), 0.f);

	Spread->AddExposure(Origin, 150.f, 6.f, TAG_Element_Fire);
	Run(Spread, 10);

	const float PartWay = Spread->GetScorchAt(Origin, TAG_Element_Fire);

	TestTrue(TEXT("Burning scorches it"), PartWay > 0.f);
	TestTrue(TEXT("But not all at once"), PartWay < 1.f);

	// THE SAME RULE THE CRATE LIVES BY, at a different granularity. Fuel is spent
	// only while the cell is alight and never regrows, so quenching stops the burn
	// where it stopped -- partially burnt grass, and nothing implements it.
	Spread->Extinguish(Origin, 400.f, TAG_Element_Fire);
	Run(Spread, 30);

	TestFalse(TEXT("The fire is out"), Spread->IsBurning(Origin, TAG_Element_Fire));
	TestEqual(TEXT("And the scorch stays exactly where it was"),
		Spread->GetScorchAt(Origin, TAG_Element_Fire), PartWay, 0.001f);

	// AND THE REMAINDER IS STILL THERE TO BURN.
	Spread->AddExposure(Origin, 150.f, 6.f, TAG_Element_Fire);
	Run(Spread, 10);

	TestTrue(TEXT("Relit, it carries on from where it was"),
		Spread->GetScorchAt(Origin, TAG_Element_Fire) > PartWay);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
