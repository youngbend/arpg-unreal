// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ARPGDischargeContext.h"
#include "ARPGDischargeEffect.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGFluidBody.h"
#include "ARPGFluidDefinition.h"
#include "ARPGFluidGeometry.h"
#include "ARPGFluidSurfaceSubsystem.h"
#include "ARPGGameplayTags.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicElement.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

/**
 * Persistent fluids.
 *
 * EVERY BODY IS A POLYGON, and that one decision is what the whole system rests
 * on: depositing is a union, rain and evaporation are offsets of opposite sign,
 * and freezing is an intersection. The geometry cases below pin those four
 * operations directly, because everything else is a consequence of them.
 *
 * The phase gate -- an ice shard freezing part of a pool into something a player
 * can stand on -- is Solidify.FreezesTheOverlapIntoAStandableSlab.
 */
namespace ARPGFluidTestUtils
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

	UARPGFluidDefinition* MakeWater(UObject* Outer, UARPGMagicElement* Element)
	{
		UARPGFluidDefinition* Definition = NewObject<UARPGFluidDefinition>(Outer);
		Definition->Element = Element;
		Definition->Depth = 20.f;
		Definition->MinimumArea = 2500.f;
		Definition->EvaporationRate = 10.f;
		Definition->RainGrowthRate = 10.f;
		Definition->MergeDistance = 200.f;
		return Definition;
	}

	/**
	 * Something for the ground probe to find.
	 *
	 * A body lies on the GROUND, and an automation world has none -- no landscape,
	 * no floor, nothing a line trace can hit. Without this every deposit correctly
	 * concludes the spell expired over a drop and wet nothing.
	 */
	AActor* MakeGround(UWorld* World, float Height, FVector2D Extent = FVector2D(5000, 5000))
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AActor* Ground = World->SpawnActor<AActor>(AActor::StaticClass(),
			FTransform(FVector(0, 0, Height)), Params);

		UBoxComponent* Box = NewObject<UBoxComponent>(Ground, TEXT("Ground"));
		Box->SetBoxExtent(FVector(Extent.X, Extent.Y, 10.f));
		Box->SetMobility(EComponentMobility::Movable);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Ground->SetRootComponent(Box);
		Box->RegisterComponent();
		Box->SetWorldLocation(FVector(0, 0, Height - 10.f)); // top face AT Height

		return Ground;
	}

	/**
	 * A cast spell, at the point it is about to finish.
	 *
	 * Spawned rather than constructed because the deposit hangs off EndPlay, which
	 * only a real actor in a real world reaches.
	 */
	AARPGDischargeEffect* MakeCastSpell(UWorld* World, UARPGMagicElement* Element,
		FVector Origin, FVector FinishedAt, float Radius, bool bSwept = false)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AARPGDischargeEffect* Effect = World->SpawnActor<AARPGDischargeEffect>(
			AARPGDischargeEffect::StaticClass(), FTransform(FinishedAt), Params);

		FARPGDischargeContext Context;
		Context.PrimaryElement = Element;
		Context.Origin = Origin;
		Context.ComputedDamage = 25.f;
		Effect->InitializeFromContext(Context);

		Effect->DepositRadius = Radius;
		Effect->bDepositSwept = bSwept;

		// EndPlay is only routed to an actor that BEGAN play, so a spell that never
		// started cannot finish and the deposit would silently never happen. The
		// fixture world has begun play and spawning should cover this; the guard is
		// here so the test is asserting on the deposit rather than on that.
		if (!Effect->HasActorBegunPlay())
		{
			Effect->DispatchBeginPlay();
		}

		// Lifespan off: these tests decide when the spell finishes.
		Effect->SetLifeSpan(0.f);

		return Effect;
	}
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidGeometryTest,
	"ARPG.World.Fluid.Geometry.AreaAndContainment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidGeometryTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidGeometry;

	const TArray<FVector2D> Square = { {0, 0}, {100, 0}, {100, 100}, {0, 100} };

	TestEqual(TEXT("A unit square's area"), PolygonArea(Square), 10000.0, 0.01);
	TestEqual(TEXT("And its perimeter"), PolygonPerimeter(Square), 400.0, 0.01);
	TestEqual(TEXT("And its centroid"), PolygonCentroid(Square), FVector2D(50, 50));

	TestTrue(TEXT("A point inside is inside"), PolygonContains(Square, FVector2D(50, 50)));
	TestFalse(TEXT("And one outside is not"), PolygonContains(Square, FVector2D(150, 50)));

	// Winding-agnostic: a ring handed back as a hole is still measurable.
	TArray<FVector2D> Reversed = Square;
	Algo::Reverse(Reversed);
	TestEqual(TEXT("Area does not care about winding"), PolygonArea(Reversed), 10000.0, 0.01);

	// A circle approximated by enough segments is close to pi r squared, and
	// slightly UNDER -- an inscribed n-gon, which is what a fresh puddle is.
	const TArray<FVector2D> Circle = MakeCircle(FVector2D::ZeroVector, 100.0, 64);
	const double CircleArea = PolygonArea(Circle);
	TestTrue(TEXT("A circle approximates pi r squared"),
		CircleArea > 31000.0 && CircleArea < PI * 100.0 * 100.0);

	// A stadium is longer than the circle it was swept from.
	const TArray<FVector2D> Stadium = MakeStadium(FVector2D(0, 0), FVector2D(200, 0), 100.0, 32);
	TestTrue(TEXT("A swept footprint is larger than a disc"),
		PolygonArea(Stadium) > CircleArea);
	TestTrue(TEXT("And covers the far end of the sweep"),
		PolygonContains(Stadium, FVector2D(200, 0)));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidBooleanTest,
	"ARPG.World.Fluid.Geometry.FourOperationsBehave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidBooleanTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidGeometry;

	const TArray<FVector2D> A = { {0, 0}, {100, 0}, {100, 100}, {0, 100} };
	const TArray<FVector2D> B = { {50, 0}, {150, 0}, {150, 100}, {50, 100} };

	// UNION -- depositing. Two overlapping squares are one wider rectangle.
	const TArray<FVector2D> Merged = MergeRings(A, B);
	TestEqual(TEXT("A union covers both"), PolygonArea(Merged), 15000.0, 1.0);

	// INTERSECTION -- freezing. Only where they actually overlap.
	const TArray<FVector2D> Overlap = IntersectRings(A, B);
	TestEqual(TEXT("An intersection is only the shared part"),
		PolygonArea(Overlap), 5000.0, 1.0);

	// DIFFERENCE -- what a pool loses.
	const TArray<FVector2D> Remaining = SubtractRings(A, B);
	TestEqual(TEXT("A difference removes the shared part"),
		PolygonArea(Remaining), 5000.0, 1.0);

	// OFFSET, both signs -- rain and evaporation are the same operation.
	const TArray<FVector2D> Grown = OffsetRing(A, 10.0);
	const TArray<FVector2D> Shrunk = OffsetRing(A, -10.0);

	TestTrue(TEXT("An outward offset grows"), PolygonArea(Grown) > PolygonArea(A));
	TestTrue(TEXT("And an inward one shrinks"), PolygonArea(Shrunk) < PolygonArea(A));

	// Eroded past nothing is EMPTY, not a degenerate sliver. That is a real
	// outcome -- a puddle that has fully evaporated.
	TestEqual(TEXT("Eroding past nothing leaves nothing"),
		PolygonArea(OffsetRing(A, -200.0)), 0.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidShrinkTest,
	"ARPG.World.Fluid.Geometry.ShrinkToAreaConservesShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidShrinkTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidGeometry;

	const TArray<FVector2D> Square = { {0, 0}, {100, 0}, {100, 100}, {0, 100} };

	const TArray<FVector2D> Halved = ShrinkToArea(Square, 5000.0);
	TestEqual(TEXT("Shrinking hits the target area exactly"),
		PolygonArea(Halved), 5000.0, 0.01);

	// About the CENTROID, so the body stays where it was rather than sliding.
	// Compared componentwise with a tolerance: the centroid is computed from the
	// scaled vertices, so exact equality would be asserting on float rounding.
	const FVector2D Centre = PolygonCentroid(Halved);
	TestEqual(TEXT("And keeps its position in X"), static_cast<float>(Centre.X), 50.f, 0.01f);
	TestEqual(TEXT("And in Y"), static_cast<float>(Centre.Y), 50.f, 0.01f);

	// UNIFORM rather than an inward offset, because an offset erodes thin necks
	// away entirely and changes the shape -- wrong when the point is only that
	// some of the fluid was used up.
	TestEqual(TEXT("A square stays a square"), Halved.Num(), 4);

	TestEqual(TEXT("Asking for more than there is changes nothing"),
		PolygonArea(ShrinkToArea(Square, 20000.0)), 10000.0, 0.01);

	return true;
}

// ---------------------------------------------------------------------------
// Pools
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidDepositTest,
	"ARPG.World.Fluid.Pools.DepositsMergeRatherThanStack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidDepositTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	if (!Fluids)
	{
		AddError(TEXT("Setup: no fluid subsystem in the test world."));
		return false;
	}

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	Fluids->Definitions = { MakeWater(GetTransientPackage(), Water) };

	AARPGFluidPool* First = Fluids->Deposit(FVector(0, 0, 0), 100.f, TAG_Element_Water);
	TestNotNull(TEXT("A deposit makes a pool"), First);
	TestEqual(TEXT("One pool exists"), Fluids->GetPools().Num(), 1);

	const double FirstArea = First->GetArea();

	// Overlapping the first: two puddles of the same thing are ONE puddle.
	// Leaving them separate would double their ambient effect and make the pair
	// react twice to the same spell.
	AARPGFluidPool* Second = Fluids->Deposit(FVector(80, 0, 0), 100.f, TAG_Element_Water);
	TestEqual(TEXT("An overlapping deposit merges"), Fluids->GetPools().Num(), 1);
	TestEqual(TEXT("Into the same pool"), Second, First);
	TestTrue(TEXT("Which grew"), First->GetArea() > FirstArea);

	// Far away: its own body, which is why islands are dropped by the merge
	// rather than smuggled back as a second ring.
	Fluids->Deposit(FVector(5000, 0, 0), 100.f, TAG_Element_Water);
	TestEqual(TEXT("A distant deposit makes its own pool"), Fluids->GetPools().Num(), 2);

	// Too little to be a body of its own: spawning it would replicate a pool that
	// the next weather tick destroys for being under the same floor.
	TestNull(TEXT("A splash below the minimum area makes no pool"),
		Fluids->Deposit(FVector(-5000, 0, 0), 10.f, TAG_Element_Water));
	TestEqual(TEXT("And leaves the count alone"), Fluids->GetPools().Num(), 2);

	// But the same splash still ADDS to one it lands in -- the floor is on being a
	// body of your own, not on being worth anything -- which is how repeated light
	// casts eventually wet the ground.
	const double BeforeSplash = First->GetArea();
	TestEqual(TEXT("A splash that small still merges into a pool it overlaps"),
		Fluids->Deposit(FVector(190, 0, 0), 25.f, TAG_Element_Water), First);
	TestTrue(TEXT("And grows it"), First->GetArea() > BeforeSplash);

	// The polygon is the truth, not the box.
	TestTrue(TEXT("A point in the pool is in the pool"),
		First->ContainsPoint(FVector(0, 0, 0)));
	TestFalse(TEXT("And one well outside is not"),
		First->ContainsPoint(FVector(1000, 1000, 0)));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidEvaporateTest,
	"ARPG.World.Fluid.Pools.EvaporationRemovesAPool",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidEvaporateTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	Fluids->Definitions = { MakeWater(GetTransientPackage(), Water) };

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 150.f, TAG_Element_Water);
	const double Initial = Pool->GetArea();

	Fluids->bRaining = false;
	Fluids->StepSimulation(1.f);

	TestTrue(TEXT("Evaporation shrinks the pool"), Pool->GetArea() < Initial);

	// The minimum area floor exists because an evaporating pool's area
	// approaches zero asymptotically -- without it a sliver would live forever,
	// costing a rebuild every tick to become imperceptibly smaller.
	for (int32 Step = 0; Step < 40; ++Step)
	{
		Fluids->StepSimulation(1.f);
	}

	TestEqual(TEXT("And eventually removes it entirely"), Fluids->GetPools().Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidRainTest,
	"ARPG.World.Fluid.Pools.RainIsEvaporationWithTheOppositeSign",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidRainTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	Fluids->Definitions = { MakeWater(GetTransientPackage(), Water) };

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 150.f, TAG_Element_Water);
	const double Initial = Pool->GetArea();

	Fluids->bRaining = true;
	Fluids->StepSimulation(1.f);

	// Literally the same operation with the sign flipped, which is the entire
	// reason a body is a polygon rather than a grid or a heightfield.
	TestTrue(TEXT("Rain grows the pool"), Pool->GetArea() > Initial);

	return true;
}

// ---------------------------------------------------------------------------
// Casting
//
// The other half of the system, and the half that was missing: everything above
// grows, erodes, freezes or melts a body that ALREADY EXISTS, and until a cast
// spell could put one there, a water spell landed on dry ground and left it dry.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidCastDepositTest,
	"ARPG.World.Fluid.Casting.ASpellLeavesABodyOnTheGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidCastDepositTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	if (!Fluids)
	{
		AddError(TEXT("Setup: no fluid subsystem in the test world."));
		return false;
	}

	MakeGround(Scope.World, 0.f);

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	Fluids->Definitions = { MakeWater(GetTransientPackage(), Water) };

	// Finishing at chest height, which is where a spell actually ends.
	AARPGDischargeEffect* Bolt = MakeCastSpell(Scope.World, Water,
		/*Origin=*/FVector(0, 0, 150), /*FinishedAt=*/FVector(400, 0, 150), /*Radius=*/120.f);

	TestEqual(TEXT("Nothing pools while the spell is still in the air"),
		Fluids->GetPools().Num(), 0);

	Bolt->Destroy();

	TestEqual(TEXT("Finishing leaves one body"), Fluids->GetPools().Num(), 1);

	if (Fluids->GetPools().Num() != 1)
	{
		return false;
	}

	AARPGFluidPool* Pool = Fluids->GetPools()[0];

	// ON THE GROUND, not at the height the spell died at. Without the probe the
	// puddle forms at chest height and floats.
	TestEqual(TEXT("At ground level"), Pool->GroundHeight, 0.f, 1.f);
	TestTrue(TEXT("And under where the spell finished"),
		Pool->ContainsPoint(FVector(400, 0, 0)));

	// A DISC, because a projectile wet the ground where it landed rather than the
	// whole line of its flight. See bDepositSwept.
	TestFalse(TEXT("But not where it was cast from"),
		Pool->ContainsPoint(FVector(0, 0, 0)));
	TestFalse(TEXT("Nor anywhere along the way"),
		Pool->ContainsPoint(FVector(200, 0, 0)));

	// Two casts into the same spot are ONE puddle, by the same merge every other
	// deposit goes through.
	MakeCastSpell(Scope.World, Water, FVector(0, 0, 150), FVector(430, 0, 150), 120.f)->Destroy();
	TestEqual(TEXT("A second cast nearby merges rather than stacking"),
		Fluids->GetPools().Num(), 1);

	// The knob is a radius and 0 is the default, so most spells leave nothing
	// without anyone having to opt them out.
	MakeCastSpell(Scope.World, Water, FVector(0, 0, 150), FVector(3000, 0, 150),
		/*Radius=*/0.f)->Destroy();
	TestEqual(TEXT("A spell with no deposit radius leaves nothing"),
		Fluids->GetPools().Num(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidCastNoDefinitionTest,
	"ARPG.World.Fluid.Casting.OnlyWhatPoolsLeavesAnything",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidCastNoDefinitionTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	MakeGround(Scope.World, 0.f);

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);

	Fluids->Definitions = { MakeWater(GetTransientPackage(), Water) };

	// NOTHING IN C++ KNOWS THAT WATER POOLS AND FIRE DOES NOT. The fire orb is
	// configured to deposit exactly as the water one is, and the only difference
	// is that no fluid definition describes fire -- so there is nothing for it to
	// leave. That is what keeps the branch out of the discharge effect and the
	// list of wet elements out of the codebase entirely.
	MakeCastSpell(Scope.World, Fire, FVector(0, 0, 150), FVector(0, 0, 150), 120.f)->Destroy();
	TestEqual(TEXT("An element with no fluid definition leaves nothing"),
		Fluids->GetPools().Num(), 0);

	MakeCastSpell(Scope.World, Water, FVector(0, 0, 150), FVector(0, 0, 150), 120.f)->Destroy();
	TestEqual(TEXT("And the same spell of an element that pools does"),
		Fluids->GetPools().Num(), 1);

	// Expired high above the floor: past MaxDepositDrop it wet nothing, which is
	// an outcome and not a failure.
	MakeCastSpell(Scope.World, Water, FVector(0, 0, 150),
		FVector(2000, 0, Fluids->MaxDepositDrop + 500.f), 120.f)->Destroy();
	TestEqual(TEXT("A spell that expired far above the ground leaves nothing"),
		Fluids->GetPools().Num(), 1);

	// And out past the floor entirely, where the probe finds no ground at all.
	MakeCastSpell(Scope.World, Water, FVector(0, 0, 150), FVector(50000, 0, 150), 120.f)->Destroy();
	TestEqual(TEXT("Nor does one that finished over a drop"), Fluids->GetPools().Num(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidCastSweptTest,
	"ARPG.World.Fluid.Casting.AnElongatedSpellLeavesASweptFootprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidCastSweptTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	MakeGround(Scope.World, 0.f);

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	Fluids->Definitions = { MakeWater(GetTransientPackage(), Water) };

	// A JET, whose body is elongated at any one instant -- so the stadium from
	// where it was cast to where it ends is its honest footprint, which a
	// projectile's line of flight is not.
	AARPGDischargeEffect* Jet = MakeCastSpell(Scope.World, Water,
		/*Origin=*/FVector(0, 0, 150), /*FinishedAt=*/FVector(600, 0, 150),
		/*Radius=*/100.f, /*bSwept=*/true);

	Jet->Destroy();

	TestEqual(TEXT("A jet leaves one body"), Fluids->GetPools().Num(), 1);

	if (Fluids->GetPools().Num() != 1)
	{
		return false;
	}

	AARPGFluidPool* Pool = Fluids->GetPools()[0];

	TestTrue(TEXT("Covering where it was cast"), Pool->ContainsPoint(FVector(0, 0, 0)));
	TestTrue(TEXT("And the whole way along"), Pool->ContainsPoint(FVector(300, 0, 0)));
	TestTrue(TEXT("And where it ended"), Pool->ContainsPoint(FVector(600, 0, 0)));
	TestFalse(TEXT("But not off to the side"), Pool->ContainsPoint(FVector(300, 600, 0)));

	// Larger than the disc the same spell would have left unswept, which is the
	// only reason the flag exists.
	TestTrue(TEXT("A swept footprint is bigger than a disc of the same radius"),
		Pool->GetArea() > PI * 100.0 * 100.0);

	return true;
}

// ---------------------------------------------------------------------------
// Freezing -- the phase gate
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidSolidifyTest,
	"ARPG.World.Fluid.Solidify.FreezesTheOverlapIntoAStandableSlab",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidSolidifyTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f; // isolate freezing from drying

	UARPGSolidDefinition* IceDefinition = NewObject<UARPGSolidDefinition>();
	IceDefinition->Element = Ice;
	IceDefinition->Thickness = 30.f;
	IceDefinition->bStandable = true;
	IceDefinition->MeltRate = 0.f;
	IceDefinition->MinimumArea = 100.f;

	// NOTHING IN C++ KNOWS THAT WATER FREEZES. This row, in the Surface scope
	// with Solidify mode, is the entire declaration -- the same table and the
	// same scoping every other relationship in the game uses.
	UARPGMagicCombinationTable* Table = NewObject<UARPGMagicCombinationTable>();
	UARPGMagicCombinationEntry* Entry = NewObject<UARPGMagicCombinationEntry>(Table);
	Entry->RequiredElements.AddTag(TAG_Element_Ice);
	Entry->RequiredElements.AddTag(TAG_Element_Water);
	Entry->Result = Ice;
	Entry->Mode = EARPGReactionMode::Solidify;
	Entry->Scope = static_cast<int32>(EARPGCombinationScope::Surface);
	Table->Entries.Add(Entry);

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { IceDefinition };
	Fluids->CombinationTable = Table;

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 300.f, TAG_Element_Water);
	TestNotNull(TEXT("Setup: a pool exists"), Pool);
	const double PoolArea = Pool->GetArea();

	// An ice shard, overlapping part of the pool but not all of it.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Shard = Scope.World->SpawnActor<AActor>(AActor::StaticClass(),
		FTransform(FVector(200, 0, 0)), Params);

	USphereComponent* Sphere = NewObject<USphereComponent>(Shard);
	Sphere->SetSphereRadius(150.f);
	Sphere->SetMobility(EComponentMobility::Movable);
	Sphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Shard->SetRootComponent(Sphere);
	Sphere->RegisterComponent();
	Sphere->SetWorldLocation(FVector(200, 0, 0));

	UARPGElementalVolumeComponent* ShardVolume = NewObject<UARPGElementalVolumeComponent>(Shard);
	ShardVolume->Element = Ice;
	ShardVolume->OverlapSource = Sphere;
	ShardVolume->SetupAttachment(Sphere);
	ShardVolume->RegisterComponent();
	ShardVolume->SetEnergy(50.f);

	const bool bFroze = Fluids->TrySolidify(Pool->Volume, ShardVolume);

	TestTrue(TEXT("Ice meeting a pool freezes it"), bFroze);
	TestEqual(TEXT("Producing one slab"), Fluids->GetSolids().Num(), 1);

	AARPGFluidSolid* Floe = Fluids->GetSolids()[0];

	// THE OVERLAP, not the whole pool and not the whole shard. Freezing exactly
	// where the two met is the whole reason a body is a polygon.
	TestTrue(TEXT("The slab is smaller than the pool was"), Floe->GetArea() < PoolArea);
	TestTrue(TEXT("But is a real slab"), Floe->GetArea() > 0.0);

	// The gate: something a player can stand on.
	TestTrue(TEXT("And you can stand on it"),
		Floe->IsStandableAt(FVector(200, 0, Floe->GetSurfaceHeight())));
	TestFalse(TEXT("But not off its edge"),
		Floe->IsStandableAt(FVector(2000, 0, 0)));

	// It sits ABOVE the water it formed on, so standing on the floe is not
	// standing in the pool.
	TestTrue(TEXT("The slab's surface is above the pool's"),
		Floe->GetSurfaceHeight() > Pool->GetSurfaceHeight());

	// The fluid is genuinely used up -- and the pool keeps its SHAPE rather than
	// gaining a hole, because a liquid flows back over a hole cut in it.
	if (Fluids->GetPools().Num() > 0)
	{
		TestTrue(TEXT("The pool lost the frozen area"), Pool->GetArea() < PoolArea);
	}

	// Spent freezing it.
	TestEqual(TEXT("The shard is spent"), ShardVolume->GetEnergy(), 0.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidNoRowTest,
	"ARPG.World.Fluid.Solidify.WithoutARowNothingFreezes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidNoRowTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);

	Fluids->Definitions = { MakeWater(GetTransientPackage(), Water) };
	Fluids->CombinationTable = NewObject<UARPGMagicCombinationTable>();

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 300.f, TAG_Element_Water);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Fireball = Scope.World->SpawnActor<AActor>(AActor::StaticClass(),
		FTransform::Identity, Params);

	USphereComponent* Sphere = NewObject<USphereComponent>(Fireball);
	Sphere->SetSphereRadius(150.f);
	Sphere->SetMobility(EComponentMobility::Movable);
	Fireball->SetRootComponent(Sphere);
	Sphere->RegisterComponent();

	UARPGElementalVolumeComponent* Volume = NewObject<UARPGElementalVolumeComponent>(Fireball);
	Volume->Element = Fire;
	Volume->OverlapSource = Sphere;
	Volume->RegisterComponent();
	Volume->SetEnergy(50.f);

	// Fire meeting water is a COLLISION, not a solidification. Handing the pair
	// straight back is what lets the reaction subsystem narrow the case without
	// ever swallowing the general one.
	TestFalse(TEXT("A pair with no Solidify row freezes nothing"),
		Fluids->TrySolidify(Pool->Volume, Volume));
	TestEqual(TEXT("So no slab is made"), Fluids->GetSolids().Num(), 0);
	TestEqual(TEXT("And the fireball is untouched"), Volume->GetEnergy(), 50.f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
