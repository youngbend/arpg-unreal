// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ARPGConductionSubsystem.h"
#include "ARPGDischargeContext.h"
#include "ARPGDischargeEffect.h"
#include "ARPGElementPalette.h"
#include "ARPGElementalReactionSubsystem.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGFluidBody.h"
#include "ARPGFluidDefinition.h"
#include "ARPGFluidGeometry.h"
#include "ARPGSolidField.h"
#include "ARPGFluidSurfaceSubsystem.h"
#include "ARPGGameplayTags.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicElement.h"
#include "ARPGReservoirVolumeComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/Material.h"

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

		// AN ORDINARY PUDDLE unless a test says otherwise. The class default is
		// 200000 square cm, which a 300-radius test pool quietly exceeds -- and a
		// pool that has become a reservoir is bottomless, so freezing stops
		// depleting it and the deposit tests would be asserting on the wrong body.
		Definition->ReservoirArea = 100000000.f;

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
	 * The Solidify row that makes ice and water freeze, and nothing else.
	 *
	 * NOTHING IN C++ KNOWS THAT WATER FREEZES -- this row, Surface-scoped, is the
	 * whole declaration, and it is the same table every other relationship uses.
	 */
	UARPGMagicCombinationTable* MakeFreezeTable(UARPGMagicElement* Ice)
	{
		UARPGMagicCombinationTable* Table = NewObject<UARPGMagicCombinationTable>();

		UARPGMagicCombinationEntry* Entry = NewObject<UARPGMagicCombinationEntry>(Table);
		Entry->RequiredElements.AddTag(TAG_Element_Ice);
		Entry->RequiredElements.AddTag(TAG_Element_Water);
		Entry->Result = Ice;
		Entry->Mode = EARPGReactionMode::Solidify;
		Entry->Scope = static_cast<int32>(EARPGCombinationScope::Surface);
		Table->Entries.Add(Entry);

		return Table;
	}

	UARPGSolidDefinition* MakeIce(UARPGMagicElement* Element)
	{
		UARPGSolidDefinition* Definition = NewObject<UARPGSolidDefinition>();
		Definition->Element = Element;
		Definition->Thickness = 30.f;
		Definition->bStandable = true;
		Definition->MeltRate = 0.f;
		Definition->MinimumArea = 100.f;
		return Definition;
	}

	/** An ice shard, as a volume with a sphere for its reach. */
	UARPGElementalVolumeComponent* MakeShard(UWorld* World, UARPGMagicElement* Ice,
		FVector At, float Radius)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(At), Params);

		USphereComponent* Sphere = NewObject<USphereComponent>(Actor);
		Sphere->SetSphereRadius(Radius);
		Sphere->SetMobility(EComponentMobility::Movable);
		Sphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Actor->SetRootComponent(Sphere);
		Sphere->RegisterComponent();
		Sphere->SetWorldLocation(At);

		UARPGElementalVolumeComponent* Volume = NewObject<UARPGElementalVolumeComponent>(Actor);
		Volume->Element = Ice;
		Volume->OverlapSource = Sphere;
		Volume->SetupAttachment(Sphere);
		Volume->RegisterComponent();
		Volume->SetEnergy(50.f);

		return Volume;
	}

	/**
	 * A river: authored, static, and NOT a fluid pool.
	 *
	 * A tall box whose waterline sits well below its top, which is how a body of
	 * water is authored -- see UARPGElementalVolumeComponent. This is the base
	 * reservoir rather than the water-body one on purpose: the freezing logic is
	 * written against ContainsPoint, so it can be exercised in full without the
	 * Water plugin, and a spline body only makes the containment more accurate.
	 */
	UARPGReservoirVolumeComponent* MakeRiver(UWorld* World, UARPGMagicElement* Water,
		FVector Centre, FVector Extent)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(Centre), Params);

		UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
		Box->SetBoxExtent(Extent);
		Box->SetMobility(EComponentMobility::Movable);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Actor->SetRootComponent(Box);
		Box->RegisterComponent();
		Box->SetWorldLocation(Centre);

		UARPGReservoirVolumeComponent* River = NewObject<UARPGReservoirVolumeComponent>(Actor);
		River->Element = Water;
		River->OverlapSource = Box;
		River->SetupAttachment(Box);
		River->RegisterComponent();
		River->SetEnergy(10000.f);

		// The waterline, relative to the volume -- well under the top of the box,
		// which reaches up so a spell arriving from above still enters.
		River->SurfaceHeightOffset = 0.f;

		return River;
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
// Reactions taking ground
//
// A reaction that spent a body's energy used to change nothing about the body.
// A fireball into a puddle made steam and left the puddle exactly as big,
// because a pool recomputes its energy from its area on the next weather tick
// and quietly discarded whatever had been spent. A floe was worse: it carried no
// energy at all, so the solver bailed at its own zero-energy guard and a fire
// spell thrown at ice did nothing whatsoever.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidBoilTest,
	"ARPG.World.Fluid.Reaction.FireBoilsAPuddleAway",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidBoilTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Steam = MakeElement(GetTransientPackage(), TAG_Element_Steam);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;   // isolate boiling from drying
	WaterDefinition->MinimumArea = 1000.f;
	WaterDefinition->EnergyPerArea = 0.001f;

	Fluids->Definitions = { WaterDefinition };

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 300.f, TAG_Element_Water);
	if (!Pool)
	{
		AddError(TEXT("Setup: no pool was deposited."));
		return false;
	}

	const double Before = Pool->GetArea();

	// Energy is area times density, so the pool is worth exactly this much.
	TestEqual(TEXT("A pool is worth its area"),
		Pool->Volume->GetEnergy(), static_cast<float>(Before * 0.001), 0.01f);

	// A fireball spending a quarter of it. Consume is the path the reaction solver
	// takes, and what it triggers is the body's own reaction hook.
	const float Spend = static_cast<float>(Before * 0.001 * 0.25);
	const FVector BeforeScale = Pool->GetActorScale3D();

	Pool->Volume->Consume(Spend, Steam);

	// THE SPEND CONVERTED BACK INTO GROUND. Which means the combination table's
	// consumption rates already decide how fast fire eats water, with no second
	// set of numbers anywhere.
	TestEqual(TEXT("A quarter of the energy is a quarter of the puddle"),
		Pool->GetArea(), Before * 0.75, Before * 0.02);

	// A REACTION TAKES GROUND, NOT SCALE. Without the hook a body falls to the
	// default projectile reaction, which shrinks the actor's transform -- leaving
	// the mesh, the trigger box and the outline disagreeing inside one frame.
	TestEqual(TEXT("And the actor is not scaled like a fireball"),
		Pool->GetActorScale3D(), BeforeScale);

	// And its energy tracks the ground it has left, rather than the two drifting
	// apart until the next weather tick resets one of them.
	TestEqual(TEXT("Energy follows the area down"),
		Pool->Volume->GetEnergy(), static_cast<float>(Pool->GetArea() * 0.001), 0.01f);

	// The rest of it. Boiled past the floor, the pool is gone -- and gone from the
	// subsystem's register too, which the default reaction's own Destroy would
	// not have managed.
	Pool->Volume->Consume(Pool->Volume->GetEnergy(), Steam);

	TestEqual(TEXT("Boiled away entirely, no pool is left"), Fluids->GetPools().Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidMeltTest,
	"ARPG.World.Fluid.Reaction.FireMeltsAFloeBackIntoWater",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidMeltTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	UARPGElementalReactionSubsystem* Reactions =
		Scope.World->GetSubsystem<UARPGElementalReactionSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);
	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);

	// A palette, so the product resolves to the shared placeholder rather than
	// warning that it would be invisible.
	Water->Palette = NewObject<UARPGElementPalette>();

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;
	WaterDefinition->MinimumArea = 1000.f;

	UARPGSolidDefinition* IceDefinition = MakeIce(Ice);
	IceDefinition->MeltsInto = WaterDefinition;
	IceDefinition->EnergyPerArea = 0.002f;

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { IceDefinition };

	// Fire + ice MELTS, in the Collision scope. Nothing in C++ knows that; this
	// row is the whole declaration, same as every other relationship.
	UARPGMagicCombinationTable* Table = NewObject<UARPGMagicCombinationTable>();
	UARPGMagicCombinationEntry* Entry = NewObject<UARPGMagicCombinationEntry>(Table);
	Entry->RequiredElements.AddTag(TAG_Element_Fire);
	Entry->RequiredElements.AddTag(TAG_Element_Ice);
	Entry->Result = Water;
	Entry->Scope = static_cast<int32>(EARPGCombinationScope::Collision);
	Table->Entries.Add(Entry);

	Reactions->CombinationTable = Table;
	Reactions->MinMagnitude = 0.f;
	Fluids->CombinationTable = Table;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AARPGFluidSolid* Floe = Scope.World->SpawnActor<AARPGFluidSolid>(
		AARPGFluidSolid::StaticClass(), FTransform::Identity, Params);

	Floe->Setup(IceDefinition, ARPGFluidGeometry::MakeCircle(FVector2D::ZeroVector, 200.0), 0.f);

	// THE THING THAT USED TO BE ZERO. A slab carried no energy -- "not a body you
	// react with" -- so Resolve bailed at its guard against zero-energy volumes
	// and a fireball thrown at ice did nothing at all.
	const float FloeEnergy = Floe->Volume->GetEnergy();
	TestTrue(TEXT("A floe is worth something to melt"), FloeEnergy > 0.f);

	// Half the floe's worth of fire.
	UARPGElementalVolumeComponent* Fireball =
		MakeShard(Scope.World, Fire, FVector(0, 0, 0), 150.f);
	Fireball->SetEnergy(FloeEnergy * 0.5f);

	const double BeforeArea = Floe->GetArea();
	Reactions->Resolve(Floe->Volume, Fireball);

	TestTrue(TEXT("Fire meeting ice melts some of it"), Floe->GetArea() < BeforeArea);
	TestSamePtr(TEXT("Producing water"), Reactions->GetLastProduct(), Water);

	// AT THE REACTION, not at the end. Melting is the one thing that gives a
	// slab's fluid back, and it gives it back as it happens -- the water is the
	// visible result of the shot, so it appears when the shot lands rather than
	// materialising later when the last sliver of ice happens to go.
	TestEqual(TEXT("Which is water on the ground, immediately"), Fluids->GetPools().Num(), 1);

	// AND ONLY ONCE. The row's Result is water too, so a water product was spawned
	// at this same contact -- and a discharge that lands deposits whatever its
	// element pools as. That is the SAME water described twice, and it made the
	// puddle bigger than the ice that produced it.
	TestTrue(TEXT("The melt is on the reaction's ledger"),
		Fluids->WasFluidReturned(TAG_Element_Water));

	int32 Products = 0;
	for (TActorIterator<AARPGDischargeEffect> It(Scope.World); It; ++It)
	{
		if (It->GetDischargeContext().DischargeType != EARPGDischargeType::Collision)
		{
			continue;
		}

		++Products;
		TestEqual(TEXT("So the product it spawned deposits nothing"),
			It->DepositRadius, 0.f);
	}

	TestEqual(TEXT("And there was a product to check"), Products, 1);

	const double AfterFirst = Fluids->GetPools()[0]->GetArea();

	// The rest of it, melted the same way. The puddle grows; a second one is not
	// invented next to the first.
	Floe->Volume->Consume(Floe->Volume->GetEnergy(), Water);

	TestEqual(TEXT("Melted through, the floe is gone"), Fluids->GetSolids().Num(), 0);
	TestEqual(TEXT("And the water it left is one body"), Fluids->GetPools().Num(), 1);
	TestTrue(TEXT("Which grew as the rest of the ice went"),
		Fluids->GetPools()[0]->GetArea() > AfterFirst);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidAmbientMeltTest,
	"ARPG.World.Fluid.Ice.MeltingInTheSunLeavesNothingBehind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidAmbientMeltTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->MinimumArea = 100.f;
	WaterDefinition->EvaporationRate = 0.f;   // so the pool's area means something

	UARPGSolidDefinition* IceDefinition = MakeIce(Ice);
	IceDefinition->MeltsInto = WaterDefinition;
	IceDefinition->MeltRate = 20.f;   // gone in a few ticks

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { IceDefinition };
	Fluids->CombinationTable = MakeFreezeTable(Ice);

	// ON A POOL, deliberately. A floe melting over a lake would leave no puddle
	// whatever the rule was, because a lake absorbs -- so it cannot tell the two
	// behaviours apart. A pool is where returned water WOULD show, as a body that
	// grew or a second one beside it, which is what makes the assertion mean
	// something. Frozen through TrySolidify so the weather tick reaches it.
	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 500.f, TAG_Element_Water);
	UARPGElementalVolumeComponent* Shard = MakeShard(Scope.World, Ice, FVector(0, 0, 0), 200.f);

	if (!Fluids->TrySolidify(Pool->Volume, Shard) || Fluids->GetSolids().Num() != 1)
	{
		AddError(TEXT("Setup: nothing froze."));
		return false;
	}

	const double PoolBefore = Pool->GetArea();

	// AMBIENT MELTING RETURNS NOTHING, and that is a decision rather than an
	// omission. A floe thinning in the sun over a minute, then a puddle appearing
	// at the instant its last sliver goes, is water arriving out of nowhere -- and
	// it would make every slab the world ever froze into a puddle it has to keep
	// forever. Fire is the case that leaves water, because someone did it and was
	// looking when it happened.
	for (int32 Tick = 0; Tick < 12 && Fluids->GetSolids().Num() > 0; ++Tick)
	{
		Fluids->StepSimulation(0.25f);
	}

	TestEqual(TEXT("The floe melted away"), Fluids->GetSolids().Num(), 0);
	TestEqual(TEXT("Leaving the one pool that was already there"),
		Fluids->GetPools().Num(), 1);
	TestEqual(TEXT("Which is no bigger for having had ice on it"),
		Pool->GetArea(), PoolBefore, 1.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidAbsorbTest,
	"ARPG.World.Fluid.Pools.WaterReturnedToTheMiddleOfAPuddleIsNotLost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidAbsorbTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;
	WaterDefinition->Depth = 20.f;

	Fluids->Definitions = { WaterDefinition };

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 400.f, TAG_Element_Water);
	const double Before = Pool->GetArea();

	// THE TRAP THIS EXISTS FOR. Returning fluid by depositing a disc is the obvious
	// implementation and it silently conserves nothing: fluid comes back where it
	// left, so the disc lands INSIDE the outline it is joining, and a union with a
	// polygon that already contains you is that polygon. The volume has nowhere to
	// go but the ring, so the ring is what grows.
	const double Volume = 200000.0;   // 10,000 square cm at 20cm deep

	TestSamePtr(TEXT("Water returned inside a pool goes into that pool"),
		Fluids->ReturnFluid(FVector2D::ZeroVector, 0.f, Volume, WaterDefinition), Pool);

	TestEqual(TEXT("And the pool grew by exactly what it was given"),
		Pool->GetArea(), Before + Volume / WaterDefinition->Depth, 200.0);

	// Beyond it, there is nothing to grow, so a body of the right size is made.
	AARPGFluidPool* Elsewhere =
		Fluids->ReturnFluid(FVector2D(50000, 0), 0.f, Volume, WaterDefinition);

	TestNotNull(TEXT("Water returned to bare ground makes a puddle"), Elsewhere);
	if (Elsewhere)
	{
		TestTrue(TEXT("A separate one"), Elsewhere != Pool);

		// Within a few percent rather than exactly: the ring is a sixteen-sided
		// polygon inscribed in the circle the area was solved for, so it comes out
		// about 2.6% under. That is the discretisation and not a loss of water.
		const double Expected = Volume / WaterDefinition->Depth;
		TestTrue(TEXT("Holding what it was given"),
			Elsewhere->GetArea() > Expected * 0.95 && Elsewhere->GetArea() <= Expected);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidReservoirMeltTest,
	"ARPG.World.Fluid.Reservoir.MeltingAFloeOnALakeJustJoinsTheLake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidReservoirMeltTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->MinimumArea = 100.f;

	UARPGSolidDefinition* IceDefinition = MakeIce(Ice);
	IceDefinition->MeltsInto = WaterDefinition;

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { IceDefinition };
	Fluids->CombinationTable = MakeFreezeTable(Ice);

	UARPGReservoirVolumeComponent* Lake =
		MakeRiver(Scope.World, Water, FVector(0, 0, 0), FVector(2000, 2000, 350));

	UARPGElementalVolumeComponent* Shard = MakeShard(Scope.World, Ice, FVector(0, 0, 0), 200.f);
	if (!Fluids->TrySolidify(Lake, Shard) || Fluids->GetSolids().Num() != 1)
	{
		AddError(TEXT("Setup: nothing froze."));
		return false;
	}

	AARPGFluidSolid* Floe = Fluids->GetSolids()[0];
	TestEqual(TEXT("Freezing a lake invents no pool"), Fluids->GetPools().Num(), 0);

	// A LAKE TAKES ITS WATER BACK AND NOTHING APPEARS. The obvious implementation
	// -- deposit wherever the ice was -- would put a puddle mesh coplanar with the
	// lake surface, z-fighting with it, in the one place a puddle is least needed.
	// A body that is bottomless when you take from it is bottomless when you give
	// back, and the floe never learns which kind of thing it is riding.
	Floe->NoteContactAt(FVector2D::ZeroVector);
	Floe->ConsumeSurfaceArea(Floe->GetArea() * 0.5);

	TestEqual(TEXT("Melting it into the lake makes no puddle on the lake"),
		Fluids->GetPools().Num(), 0);

	return true;
}

// ---------------------------------------------------------------------------
// The slab as a solid
//
// A floe is a HEIGHTFIELD, not an outline with a thickness -- see FARPGSolidField.
// Everything interesting that happens to ice happens in the third dimension: a
// bowl melted at an angle into one edge, a step where new ice formed at the
// waterline a load had pushed the surface down to, a hole where the two faces
// met. None of it can be said with a polygon.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSolidFieldMeltTest,
	"ARPG.World.Fluid.Ice.FireMeltsABowlAndBreaksThrough",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSolidFieldMeltTest::RunTest(const FString& Parameters)
{
	FARPGSolidField Field;
	Field.BuildFrom(ARPGFluidGeometry::MakeCircle(FVector2D::ZeroVector, 300.0),
		/*CellSize=*/20.f, /*Thickness=*/30.f);

	if (!Field.IsValidField() || Field.SolidCellCount() == 0)
	{
		AddError(TEXT("Setup: the field did not build."));
		return false;
	}

	TestEqual(TEXT("Fresh ice is its full thickness throughout"),
		Field.ThicknessAt(Field.CellAt(FVector2D::ZeroVector).X,
			Field.CellAt(FVector2D::ZeroVector).Y), 30.f, 0.2f);

	// A shallow bowl in the middle. Not deep enough to break through.
	const double Removed = Field.MeltBowl(FVector2D::ZeroVector, 100.f, 10.f);

	TestTrue(TEXT("Melting takes ice away"), Removed > 0.0);
	TestTrue(TEXT("But a shallow bowl does not break through"),
		Field.IsSolidAt(FVector2D::ZeroVector));

	// A BOWL, NOT A CYLINDER, which is the whole reason an impact at the edge cuts
	// at an angle: the centre goes deepest and it tapers to nothing at the rim.
	const float AtCentre = Field.TopAt(FVector2D::ZeroVector);
	const float Midway = Field.TopAt(FVector2D(70, 0));
	const float Outside = Field.TopAt(FVector2D(200, 0));

	TestTrue(TEXT("The centre of the bowl is lowest"), AtCentre < Midway);
	TestTrue(TEXT("And it slopes up to the untouched ice"), Midway < Outside);
	TestEqual(TEXT("Which is still at full height"), Outside, 30.f, 0.2f);

	// Deep enough and the top meets the bottom. A HOLE IS NOT A THING ANYONE
	// TRACKS -- it is a cell with no ice left, which is why it cannot be bridged
	// into an outline, cannot leave a slit for an offsetter to round into arcs,
	// and cannot be dropped for not being the largest.
	Field.MeltBowl(FVector2D::ZeroVector, 100.f, 60.f);

	TestFalse(TEXT("A deep bowl melts clean through"), Field.IsSolidAt(FVector2D::ZeroVector));
	TestTrue(TEXT("And the ice around it is untouched"), Field.IsSolidAt(FVector2D(200, 0)));

	// TWO HOLES BOTH SURVIVE. Only the largest used to be kept, so a second
	// fireball made the first hole vanish.
	Field.MeltBowl(FVector2D(200, 0), 60.f, 60.f);

	TestFalse(TEXT("A second hole opens"), Field.IsSolidAt(FVector2D(200, 0)));
	TestFalse(TEXT("And the first is still there"), Field.IsSolidAt(FVector2D::ZeroVector));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSolidFieldRefreezeTest,
	"ARPG.World.Fluid.Ice.RefreezingLeavesAStepWhereTheIceWasLow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSolidFieldRefreezeTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Whole = ARPGFluidGeometry::MakeCircle(FVector2D::ZeroVector, 300.0);

	FARPGSolidField Field;
	Field.BuildFrom(Whole, /*CellSize=*/20.f, /*Thickness=*/30.f);

	// Zero is the underside the slab started at, so fresh ice tops out at its
	// thickness and the waterline sits at whatever the draft is.
	TestEqual(TEXT("Fresh ice tops out at its thickness"),
		Field.TopAt(FVector2D::ZeroVector), 30.f, 0.2f);

	// Melt a dish, then let it refreeze at a waterline BELOW the original surface
	// -- which is what a floe carrying a load presents to the water.
	Field.MeltBowl(FVector2D(150, 0), 120.f, 20.f);

	const float Dished = Field.TopAt(FVector2D(150, 0));
	TestTrue(TEXT("Setup: the dish is below the surface"), Dished < 25.f);

	Field.Resolidify(Whole, /*SurfaceZ=*/22.f, /*MinimumGain=*/0.5f);

	// THE STEP. New ice forms at the surface of the water, so a floe riding low
	// gains ice BELOW the ice that froze when it was riding high -- and the
	// difference stays in the slab when the load comes off and it rises again.
	TestEqual(TEXT("The dish refroze up to the waterline"),
		Field.TopAt(FVector2D(150, 0)), 22.f, 0.2f);
	TestEqual(TEXT("And the ice already proud of it is untouched"),
		Field.TopAt(FVector2D(-200, 0)), 30.f, 0.2f);

	TestTrue(TEXT("So the surface has a step in it"),
		Field.TopAt(FVector2D(-200, 0)) > Field.TopAt(FVector2D(150, 0)));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSolidFieldBoundedTest,
	"ARPG.World.Fluid.Ice.DetailIsBoundedNoMatterHowMuchHappens",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSolidFieldBoundedTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);
	UARPGSolidDefinition* IceDefinition = MakeIce(Ice);
	IceDefinition->CellSize = 25.f;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AARPGFluidSolid* Floe = Scope.World->SpawnActor<AARPGFluidSolid>(
		AARPGFluidSolid::StaticClass(), FTransform::Identity, Params);

	Floe->Setup(IceDefinition, ARPGFluidGeometry::MakeCircle(FVector2D::ZeroVector, 300.0), 0.f);

	const int32 Fresh = Floe->GetSurfaceTriangleCount();
	TestTrue(TEXT("A fresh floe has a surface"), Fresh > 0);

	// THE GODOT PATHOLOGY, REPRODUCED AND REFUSED. Eighteen melt ticks with holes
	// in the slab took fifty vertices to seven thousand there, because every
	// erosion offset a ring that had a bridged hole in it and the offsetter
	// rounded the slit's ends into new arcs each time. Nothing here can grow: the
	// mesh is a fixed few triangles per cell, and the cell count never changes.
	for (int32 Tick = 0; Tick < 40; ++Tick)
	{
		Floe->MeltAt(FVector2D(Tick * 7 - 140, (Tick % 5) * 40 - 80), 50.f, 4.f);
		Floe->MeltUniformly(0.2f, 0.2f);
	}

	const int32 After = Floe->GetSurfaceTriangleCount();

	TestTrue(TEXT("Forty melt ticks later there is still ice"), After > 0);
	TestTrue(TEXT("And the mesh never grew past what the grid allows"), After <= Fresh);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSolidFieldCacheTest,
	"ARPG.World.Fluid.Ice.TotalsStayTrueWithoutBeingSweptFor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSolidFieldCacheTest::RunTest(const FString& Parameters)
{
	// Area, volume, centroid and occupancy are cached rather than swept for on
	// every read -- the buoyancy tick wants all four every frame, and sweeping a
	// 2500-cell floe four times a frame was the single most expensive thing in
	// this system. Cheap is only useful if it is also RIGHT, so this walks the
	// grid by hand and compares.
	auto SweptCellCount = [](const FARPGSolidField& Field)
	{
		int32 Count = 0;
		for (int32 Y = 0; Y < Field.CountY; ++Y)
		{
			for (int32 X = 0; X < Field.CountX; ++X)
			{
				Count += Field.IsSolid(X, Y) ? 1 : 0;
			}
		}
		return Count;
	};

	FARPGSolidField Field;
	Field.BuildFrom(ARPGFluidGeometry::MakeCircle(FVector2D::ZeroVector, 300.0), 20.f, 30.f);

	TestEqual(TEXT("A fresh field's count is its cells"),
		Field.SolidCellCount(), SweptCellCount(Field));
	TestEqual(TEXT("And its volume is area times thickness"),
		Field.SolidVolume(), Field.SolidArea() * 30.0, Field.SolidArea() * 0.02);

	// Melting through changes the count, so the cache has to move with it.
	Field.MeltBowl(FVector2D::ZeroVector, 120.f, 60.f);

	TestEqual(TEXT("Melting through updates the count"),
		Field.SolidCellCount(), SweptCellCount(Field));
	TestTrue(TEXT("Which went down"), Field.SolidCellCount() < Field.CountX * Field.CountY);

	// Thinning changes volume without changing occupancy.
	const int32 BeforeCells = Field.SolidCellCount();
	const double BeforeVolume = Field.SolidVolume();

	Field.MeltUniform(2.f, 0.f);

	TestEqual(TEXT("Thinning leaves the count alone"), Field.SolidCellCount(), BeforeCells);
	TestTrue(TEXT("But takes volume"), Field.SolidVolume() < BeforeVolume);

	// TRANSLATION MOVES THE CENTROID WITHOUT A SWEEP, which is the whole reason
	// drifting is one vector add rather than a rebuild.
	const FVector2D Before = Field.SolidCentroid();
	Field.Translate(FVector2D(500, -250));

	TestEqual(TEXT("Sliding the field slides its centroid"),
		Field.SolidCentroid(), Before + FVector2D(500, -250));
	TestEqual(TEXT("And costs it no cells"), Field.SolidCellCount(), BeforeCells);

	return true;
}

// ---------------------------------------------------------------------------
// Floating
//
// A floe RIDES the water rather than being pinned to where the waterline was
// when it formed. Archimedes for the depth, the body's own current for the
// drift, and the shore for whether it may drift at all.
//
// Kinematic on purpose. UBuoyancyComponent drives a SIMULATING body, which is
// right for a boat you ride and wrong for a platform you walk on -- a simulating
// body under a character movement component jitters and gets shoved, and it
// needs simple collision where a floe's whole value is complex-as-simple
// collision honouring its outline and its melted-through hole. The water state
// still comes from the plugin; only the integration is ours.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFloatTest,
	"ARPG.World.Fluid.Floating.AFloeRidesAtItsDraft",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFloatTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;

	UARPGSolidDefinition* IceDefinition = MakeIce(Ice);
	IceDefinition->Thickness = 30.f;
	IceDefinition->Density = 0.0006f;   // 60% of water, so 60% of it sits under
	IceDefinition->SettleSpeed = 1000.f; // effectively instant, for the assertion

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { IceDefinition };
	Fluids->CombinationTable = MakeFreezeTable(Ice);

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 400.f, TAG_Element_Water);
	const float Waterline = Pool->GetSurfaceHeight();

	UARPGElementalVolumeComponent* Shard = MakeShard(Scope.World, Ice, FVector(0, 0, 0), 200.f);
	if (!Fluids->TrySolidify(Pool->Volume, Shard) || Fluids->GetSolids().Num() != 1)
	{
		AddError(TEXT("Setup: nothing froze."));
		return false;
	}

	AARPGFluidSolid* Floe = Fluids->GetSolids()[0];

	// It knows what it is riding, which is what lets it ask for a waterline that
	// moves rather than remembering one that does not.
	TestNotNull(TEXT("A floe knows the water it froze out of"), Floe->FloatsOn.GetObject());

	Floe->Tick(1.f);

	// ARCHIMEDES. A floating body displaces its own weight, so the slab's own
	// draft is Thickness * Density / WaterDensity and the area cancels out --
	// 30cm of ice at 60% of water's density rides 18cm under.
	TestEqual(TEXT("It rides at the depth its density says"), Floe->Draft, 18.f, 0.5f);

	// Which leaves the walkable top PROUD of the water rather than awash. That is
	// the whole reason Density is tuned below real ice's 92%.
	TestTrue(TEXT("And its surface stands above the waterline"),
		Floe->GetSurfaceHeight() > Waterline);
	TestEqual(TEXT("By the freeboard the draft leaves"),
		Floe->GetSurfaceHeight(), Waterline + 12.f, 0.5f);

	// A BIGGER SLAB OF THE SAME ICE RIDES THE SAME DEPTH, because the area
	// cancels. Falls out of the equation rather than being arranged.
	const double Before = Floe->GetArea();
	Floe->SetRing(ARPGFluidGeometry::MakeCircle(FVector2D::ZeroVector, 300.0));
	TestTrue(TEXT("Setup: it did get bigger"), Floe->GetArea() > Before);

	Floe->Tick(1.f);
	TestEqual(TEXT("A larger floe of the same ice rides just as deep"),
		Floe->Draft, 18.f, 0.5f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidSinkTest,
	"ARPG.World.Fluid.Floating.WeightPushesAFloeDown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidSinkTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;

	UARPGSolidDefinition* IceDefinition = MakeIce(Ice);
	IceDefinition->Thickness = 30.f;
	IceDefinition->Density = 0.0006f;
	IceDefinition->SettleSpeed = 1000.f;

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { IceDefinition };
	Fluids->CombinationTable = MakeFreezeTable(Ice);

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 400.f, TAG_Element_Water);
	UARPGElementalVolumeComponent* Shard = MakeShard(Scope.World, Ice, FVector(0, 0, 0), 200.f);
	Fluids->TrySolidify(Pool->Volume, Shard);

	if (Fluids->GetSolids().Num() != 1)
	{
		AddError(TEXT("Setup: nothing froze."));
		return false;
	}

	AARPGFluidSolid* Floe = Fluids->GetSolids()[0];
	Floe->Tick(1.f);

	const float Unloaded = Floe->Draft;
	TestEqual(TEXT("Setup: nobody is aboard"), Floe->GetOccupantCount(), 0);

	// A pawn standing on it. ECC_Pawn, because that is the one object type the
	// occupancy probe asks for -- what stands on ice is a character, and a crate
	// resting on it is not something this models.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Pawn = Scope.World->SpawnActor<AActor>(AActor::StaticClass(),
		FTransform(FVector(0, 0, Floe->GetSurfaceHeight() + 40.f)), Params);

	UCapsuleComponent* Capsule = NewObject<UCapsuleComponent>(Pawn);
	Capsule->SetCapsuleSize(40.f, 90.f);
	Capsule->SetMobility(EComponentMobility::Movable);
	Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Capsule->SetCollisionObjectType(ECC_Pawn);
	Pawn->SetRootComponent(Capsule);
	Capsule->RegisterComponent();
	Capsule->SetWorldLocation(FVector(0, 0, Floe->GetSurfaceHeight() + 40.f));

	Floe->Tick(1.f);

	TestEqual(TEXT("Someone standing on it is aboard"), Floe->GetOccupantCount(), 1);
	TestTrue(TEXT("And the ice gives under them"), Floe->Draft > Unloaded);

	// It never goes further under than it is thick. Past that the slab is swamped,
	// and letting it keep sinking would drag whoever is on it through the floor.
	TestTrue(TEXT("But never further under than it is thick"),
		Floe->Draft <= IceDefinition->Thickness + KINDA_SMALL_NUMBER);

	// Standing off the edge is standing in the water beside it, not aboard. The
	// box is broadphase; the polygon is the truth, the same split everything else
	// in this system runs on.
	Capsule->SetWorldLocation(FVector(2000, 0, Floe->GetSurfaceHeight() + 40.f));
	Floe->Tick(1.f);

	TestEqual(TEXT("Someone off the floe is not aboard"), Floe->GetOccupantCount(), 0);
	TestEqual(TEXT("And it comes back up"), Floe->Draft, Unloaded, 0.5f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidDriftTest,
	"ARPG.World.Fluid.Floating.ACurrentCarriesAFloeUnlessTheShoreHasIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidDriftTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	UARPGSolidDefinition* IceDefinition = MakeIce(Ice);
	IceDefinition->DriftResponse = 1.f;   // travel at exactly the water's speed

	Fluids->Solids = { IceDefinition };
	Fluids->CombinationTable = MakeFreezeTable(Ice);

	// A long river, 400 to either side of centre, with a current down its length.
	UARPGReservoirVolumeComponent* River =
		MakeRiver(Scope.World, Water, FVector(0, 0, 0), FVector(4000, 400, 350));
	River->FlowVelocity = FVector2D(100.f, 0.f);   // 1 m/s downstream

	// A floe well short of either bank: open water on both sides, so free to move.
	UARPGElementalVolumeComponent* Shard = MakeShard(Scope.World, Ice, FVector(0, 0, 0), 150.f);
	if (!Fluids->TrySolidify(River, Shard) || Fluids->GetSolids().Num() != 1)
	{
		AddError(TEXT("Setup: nothing froze."));
		return false;
	}

	AARPGFluidSolid* Raft = Fluids->GetSolids()[0];

	TestFalse(TEXT("A floe with open water around it is not anchored"), Raft->bAnchored);

	const double StartX = ARPGFluidGeometry::PolygonCentroid(Raft->GetRing()).X;
	Raft->Tick(1.f);
	const double DriftedX = ARPGFluidGeometry::PolygonCentroid(Raft->GetRing()).X;

	// One second at 1 m/s. Downstream, and by the water's own speed because
	// DriftResponse is 1.
	TestEqual(TEXT("The current carries it downstream"), DriftedX - StartX, 100.0, 5.0);

	// SO IT CARRIES THE PLAYER. A character on a kinematic base is moved by the
	// movement component's based movement, and the base's velocity is what it
	// imparts on jumping off. Without it the ice slides out from under them, which
	// is worse than not drifting at all.
	TestTrue(TEXT("And reports the velocity it is moving at"),
		Raft->GetSurfaceComponent()->GetComponentVelocity().X > 50.f);

	// NOW A PLUG. Frozen across the whole width, it is braced on both banks and
	// the current has nothing to push against -- which is what a spell that
	// freezes an entire river makes.
	UARPGElementalVolumeComponent* Wide = MakeShard(Scope.World, Ice, FVector(2000, 0, 0), 900.f);
	if (!Fluids->TrySolidify(River, Wide) || Fluids->GetSolids().Num() != 2)
	{
		AddError(TEXT("Setup: the wide shard froze nothing."));
		return false;
	}

	AARPGFluidSolid* Plug = Fluids->GetSolids()[1];

	TestTrue(TEXT("A floe spanning the water is anchored by the shore"), Plug->bAnchored);

	const double PlugX = ARPGFluidGeometry::PolygonCentroid(Plug->GetRing()).X;
	Plug->Tick(1.f);

	TestEqual(TEXT("And the current cannot move it"),
		ARPGFluidGeometry::PolygonCentroid(Plug->GetRing()).X, PlugX, 0.01);
	TestEqual(TEXT("Nor does it report a velocity to carry anyone"),
		Plug->GetSurfaceComponent()->GetComponentVelocity().X, 0.f);

	// It still RIDES, though -- there is water under it, and being wedged is about
	// going nowhere horizontally.
	TestTrue(TEXT("But it still floats"), Plug->Draft > 0.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFloeStrandedTest,
	"ARPG.World.Fluid.Floating.AFloeGoesWithTheWaterUnderIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFloeStrandedTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);
	UARPGMagicElement* Steam = MakeElement(GetTransientPackage(), TAG_Element_Steam);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;
	WaterDefinition->MinimumArea = 1000.f;

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { MakeIce(Ice) };
	Fluids->CombinationTable = MakeFreezeTable(Ice);

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 300.f, TAG_Element_Water);
	UARPGElementalVolumeComponent* Shard = MakeShard(Scope.World, Ice, FVector(0, 0, 0), 120.f);
	Fluids->TrySolidify(Pool->Volume, Shard);

	if (Fluids->GetSolids().Num() != 1)
	{
		AddError(TEXT("Setup: nothing froze."));
		return false;
	}

	// Boil the pool out from under it, which fire can now do.
	Pool->Volume->Consume(Pool->Volume->GetEnergy(), Steam);

	TestEqual(TEXT("The pool is gone"), Fluids->GetPools().Num(), 0);

	// AND SO IS THE ICE. Nothing linked a floe's life to the water under it, so
	// this used to leave a slab hanging in mid-air over dry ground. A floe is not
	// an independent object -- it is a thing riding a surface.
	TestEqual(TEXT("And the floe that was riding it goes with it"),
		Fluids->GetSolids().Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidHeavySolidTest,
	"ARPG.World.Fluid.Floating.ASolidTooHeavyToFloatRestsOnTheBed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidHeavySolidTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Earth = MakeElement(GetTransientPackage(), TAG_Element_Earth);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;
	WaterDefinition->Depth = 40.f;
	WaterDefinition->Density = 0.001f;

	// A CRUST, not a floe. Denser than the fluid it formed out of, so nothing here
	// should float it -- and nothing in the floating code names ice or water, so
	// the only thing that decides is which density is larger.
	UARPGSolidDefinition* Crust = NewObject<UARPGSolidDefinition>();
	Crust->Element = Earth;
	Crust->Thickness = 30.f;
	Crust->bStandable = true;
	Crust->MinimumArea = 100.f;
	Crust->SettleSpeed = 1000.f;
	Crust->Density = 0.0025f;   // heavier than the water under it

	// Permanent rock, in both of the senses that are separate questions: time does
	// not take it, and no reaction can eat it either.
	Crust->MeltRate = 0.f;
	Crust->EnergyPerArea = 0.f;

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { Crust };
	Fluids->CombinationTable = MakeFreezeTable(Earth);

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 400.f, TAG_Element_Water);
	const float Bed = Pool->GroundHeight;

	UARPGElementalVolumeComponent* Agent = MakeShard(Scope.World, Earth, FVector(0, 0, 0), 200.f);
	if (!Fluids->TrySolidify(Pool->Volume, Agent) || Fluids->GetSolids().Num() != 1)
	{
		AddError(TEXT("Setup: nothing solidified."));
		return false;
	}

	AARPGFluidSolid* Slab = Fluids->GetSolids()[0];
	Slab->Tick(1.f);

	TestTrue(TEXT("A slab denser than its fluid does not float"), Slab->bAground);

	// It comes to rest ON THE BOTTOM rather than sitting awash at the surface,
	// which is what clamping the draft to the thickness would have given.
	TestEqual(TEXT("It settles onto the bed"),
		Slab->GetActorLocation().Z, Bed, 1.f);

	// And nothing melts it. MeltRate and EnergyPerArea are the two independent
	// questions -- does time take it, can a reaction take it -- and permanent rock
	// answers no to both.
	const double Before = Slab->GetArea();
	Slab->NoteContactAt(FVector2D::ZeroVector);
	Slab->Volume->Consume(500.f, nullptr);

	TestEqual(TEXT("And no reaction can eat permanent rock"), Slab->GetArea(), Before, 1.0);
	TestEqual(TEXT("So it is still there"), Fluids->GetSolids().Num(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidBudgetTest,
	"ARPG.World.Fluid.Pools.TheWorldKeepsOnlySoManyBodies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidBudgetTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;
	WaterDefinition->MergeDistance = 0.f;   // every deposit is its own body
	WaterDefinition->MinimumArea = 100.f;

	Fluids->Definitions = { WaterDefinition };
	Fluids->MaxBodiesOfEachKind = 5;

	// NOTHING ELSE BOUNDS THIS. A deposit that merges is free; one that lands
	// clear of every pool spawns another actor with a mesh, a collider and a
	// replicated outline. A player walking a field casting water makes one per
	// cast, forever.
	for (int32 Index = 0; Index < 12; ++Index)
	{
		Fluids->Deposit(FVector(Index * 2000, 0, 0), 100.f + Index * 10.f, TAG_Element_Water);
	}

	TestEqual(TEXT("Twelve deposits made twelve bodies"), Fluids->GetPools().Num(), 12);

	// The budget is applied on the simulation step rather than at the moment of
	// depositing, so a burst of casts is never refused mid-fight -- it settles.
	Fluids->StepSimulation(0.1f);

	TestEqual(TEXT("The next step brings it back inside the budget"),
		Fluids->GetPools().Num(), 5);

	// AND THE SMALLEST WENT. Cheapest to lose, least likely to be the one someone
	// is standing in, and the deposits above got larger as they went.
	for (AARPGFluidPool* Kept : Fluids->GetPools())
	{
		TestTrue(TEXT("What survived is one of the larger bodies"),
			Kept->GetArea() > ARPGFluidGeometry::PolygonArea(
				ARPGFluidGeometry::MakeCircle(FVector2D::ZeroVector, 150.0)));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidBudgetRiderTest,
	"ARPG.World.Fluid.Pools.CullingAPoolTakesWhatWasFloatingOnIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidBudgetRiderTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;
	WaterDefinition->MergeDistance = 0.f;
	WaterDefinition->MinimumArea = 100.f;

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { MakeIce(Ice) };
	Fluids->CombinationTable = MakeFreezeTable(Ice);

	// The one that will be culled: smallest, and carrying a floe.
	AARPGFluidPool* Doomed = Fluids->Deposit(FVector(0, 0, 0), 120.f, TAG_Element_Water);
	UARPGElementalVolumeComponent* Shard = MakeShard(Scope.World, Ice, FVector(0, 0, 0), 100.f);
	if (!Fluids->TrySolidify(Doomed->Volume, Shard) || Fluids->GetSolids().Num() != 1)
	{
		AddError(TEXT("Setup: nothing froze."));
		return false;
	}

	for (int32 Index = 1; Index <= 3; ++Index)
	{
		Fluids->Deposit(FVector(Index * 3000, 0, 0), 400.f, TAG_Element_Water);
	}

	Fluids->MaxBodiesOfEachKind = 3;
	Fluids->StepSimulation(0.1f);

	TestEqual(TEXT("The smallest pool was culled"), Fluids->GetPools().Num(), 3);
	TestFalse(TEXT("And it was the one carrying the floe"),
		Fluids->GetPools().Contains(Doomed));

	// A CULL IS NOT A RETIREMENT, but a floe is not an independent object either.
	// Dropping the water and leaving the ice hanging over dry ground is the one
	// visible artefact this whole economy could produce.
	TestEqual(TEXT("The floe went with the water it was riding"),
		Fluids->GetSolids().Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidSignificanceTest,
	"ARPG.World.Fluid.Pools.DistantBodiesStopPayingForThemselves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidSignificanceTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	Fluids->SignificanceDistance = 1000.f;

	// NOBODY WATCHING MEANS EVERYTHING MATTERS. A dedicated server with no local
	// viewer -- and every fixture in this file -- would otherwise quietly switch
	// the whole system off, which is an optimisation that shows up as tests
	// passing for the wrong reason.
	Fluids->StepSimulation(0.1f);

	TestTrue(TEXT("With no viewer at all, everything is significant"),
		Fluids->IsSignificantAt(FVector(100000, 0, 0)));

	return true;
}

// ---------------------------------------------------------------------------
// Conduction through real bodies
//
// The phase 6 gate is "lightning floods a puddle chain and hurts a second
// player standing in it", and the conduction cases that prove the chain do it
// with hand-built volumes whose Conductivity they set themselves. Nothing ever
// set it on a deposited pool -- the volume's default is 0 and no definition
// carried the number -- so the gate did not hold for actual puddles.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidConductChainTest,
	"ARPG.World.Fluid.Conduction.APuddleChainCarriesACharge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidConductChainTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	UARPGElementalReactionSubsystem* Reactions =
		Scope.World->GetSubsystem<UARPGElementalReactionSubsystem>();
	UARPGConductionSubsystem* Conduction = Scope.World->GetSubsystem<UARPGConductionSubsystem>();

	if (!Conduction)
	{
		AddError(TEXT("Setup: no conduction subsystem in the test world."));
		return false;
	}

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Lightning = MakeElement(GetTransientPackage(), TAG_Element_Lightning);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;

	// Zero, so three deposits stay three BODIES rather than merging into one --
	// the chain is the point, and a merge would make it a single puddle.
	WaterDefinition->MergeDistance = 0.f;

	Fluids->Definitions = { WaterDefinition };

	// Lightning TRAVELS THROUGH water rather than reacting with it. One row.
	UARPGMagicCombinationTable* Table = NewObject<UARPGMagicCombinationTable>();
	UARPGMagicCombinationEntry* Entry = NewObject<UARPGMagicCombinationEntry>(Table);
	Entry->RequiredElements.AddTag(TAG_Element_Lightning);
	Entry->RequiredElements.AddTag(TAG_Element_Water);
	Entry->Result = Lightning;
	Entry->Mode = EARPGReactionMode::Conduct;
	Entry->Scope = static_cast<int32>(EARPGCombinationScope::Collision);
	Table->Entries.Add(Entry);

	Reactions->CombinationTable = Table;
	Conduction->CombinationTable = Table;
	Conduction->DistanceLoss = 0.f;   // isolate the per-hop conductivity term

	// Three puddles in a row, each overlapping the next. Nothing authors the
	// connection -- touching IS the connection.
	AARPGFluidPool* Near = Fluids->Deposit(FVector(0, 0, 0), 100.f, TAG_Element_Water);
	AARPGFluidPool* Middle = Fluids->Deposit(FVector(150, 0, 0), 100.f, TAG_Element_Water);
	AARPGFluidPool* Far = Fluids->Deposit(FVector(300, 0, 0), 100.f, TAG_Element_Water);

	if (Fluids->GetPools().Num() != 3)
	{
		AddError(TEXT("Setup: expected three separate pools."));
		return false;
	}

	// THE THING THAT WAS MISSING, and nobody sets it here: it comes off the
	// definition, which is the only place a designer could ever have put it.
	TestTrue(TEXT("A deposited pool carries a charge without anyone saying so"),
		Near->Volume->Conductivity > 0.f);

	// NOTHING PRIMES THE OVERLAPS HERE, and that is the point of the graph:
	// GetOverlappingVolumes runs a live physics query rather than reading the
	// component's cached overlap list. The cache is maintained as a side effect of
	// MOVEMENT and so is empty for anything standing still -- which is every
	// puddle in the game.
	const double NearArea = Near->GetArea();

	UARPGElementalVolumeComponent* Bolt =
		MakeShard(Scope.World, Lightning, FVector(0, 0, 0), 100.f);
	Bolt->SetEnergy(200.f);

	Reactions->Resolve(Bolt, Near->Volume);

	// The gate: the charge runs the WHOLE chain, from the puddle it struck to the
	// one two hops away.
	TestEqual(TEXT("The charge floods every touching puddle"),
		Conduction->GetLastReachedCount(), 3);

	// A MEDIUM IS A CARRIER, NOT A REACTANT. The bolt is spent delivering itself
	// into the water; the water is neither consumed nor boiled -- which is what
	// went wrong when Conductivity was zero, because the pair then fell through to
	// an ordinary energy trade and ate the puddle instead of running through it.
	TestEqual(TEXT("The bolt is spent entering the water"), Bolt->GetEnergy(), 0.f);
	TestEqual(TEXT("And the puddle it entered is untouched"), Near->GetArea(), NearArea, 1.0);
	TestNull(TEXT("With no product popped at the contact"), Reactions->GetLastProduct());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidNotThroughIceTest,
	"ARPG.World.Fluid.Conduction.NotThroughTheIce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidNotThroughIceTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	UARPGElementalReactionSubsystem* Reactions =
		Scope.World->GetSubsystem<UARPGElementalReactionSubsystem>();
	UARPGConductionSubsystem* Conduction = Scope.World->GetSubsystem<UARPGConductionSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);
	UARPGMagicElement* Lightning = MakeElement(GetTransientPackage(), TAG_Element_Lightning);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;

	// Both relationships in the one table, because both are relationships: the
	// same asset says ice freezes water and lightning travels through it.
	UARPGMagicCombinationTable* Table = MakeFreezeTable(Ice);

	UARPGMagicCombinationEntry* Conducts = NewObject<UARPGMagicCombinationEntry>(Table);
	Conducts->RequiredElements.AddTag(TAG_Element_Lightning);
	Conducts->RequiredElements.AddTag(TAG_Element_Water);
	Conducts->Result = Lightning;
	Conducts->Mode = EARPGReactionMode::Conduct;
	Conducts->Scope = static_cast<int32>(EARPGCombinationScope::Collision);
	Table->Entries.Add(Conducts);

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { MakeIce(Ice) };
	Fluids->CombinationTable = Table;
	Reactions->CombinationTable = Table;
	Conduction->CombinationTable = Table;

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 300.f, TAG_Element_Water);

	// Freeze the middle of it into a floe you could stand on.
	UARPGElementalVolumeComponent* Shard = MakeShard(Scope.World, Ice, FVector(0, 0, 0), 150.f);
	if (!Fluids->TrySolidify(Pool->Volume, Shard))
	{
		AddError(TEXT("Setup: nothing froze."));
		return false;
	}

	// A bolt striking the ice, not the water.
	UARPGElementalVolumeComponent* Bolt =
		MakeShard(Scope.World, Lightning, FVector(0, 0, 0), 60.f);
	Bolt->SetEnergy(200.f);

	Reactions->Resolve(Bolt, Pool->Volume);

	// WHAT IT HIT WAS THE ICE. A floe roofs the water beneath it, and the pool's
	// own collider knows nothing about that -- so without the check the bolt
	// enters the water under the slab and floods it. Standing on the floe is how
	// you cross an electrified pool.
	//
	// Asked for ANY medium now, not only a reservoir: a floe forms on whatever it
	// froze, and a pool is only a reservoir once it has gathered past a threshold,
	// so gating on that roofed nothing in the ordinary case.
	TestEqual(TEXT("A bolt that struck the ice conducts nowhere"),
		Conduction->GetLastReachedCount(), 0);

	return true;
}

// ---------------------------------------------------------------------------
// Reservoirs
//
// A river is AUTHORED and STATIC where a puddle is spawned and reshaped, so it
// is not a fluid pool and should not become one. What makes it part of the
// elemental world is a volume saying what it is made of -- and until this
// landed, the one thing it could not do was freeze.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFreezeRiverTest,
	"ARPG.World.Fluid.Reservoir.ARiverFreezesWithoutRunningOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFreezeRiverTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	Fluids->Solids = { MakeIce(Ice) };
	Fluids->CombinationTable = MakeFreezeTable(Ice);

	// Wide, and 7m deep as the original was, with the waterline at the volume's
	// own height rather than the top of the box.
	UARPGReservoirVolumeComponent* River =
		MakeRiver(Scope.World, Water, FVector(0, 0, 0), FVector(2000, 400, 350));

	// The defaults are what being a reservoir MEANS, rather than a checklist.
	TestTrue(TEXT("A river is bottomless"), River->bReservoir);
	TestTrue(TEXT("And something you stand in"), River->bAmbientSource);
	TestTrue(TEXT("And carries a charge"), River->Conductivity > 0.f);

	UARPGElementalVolumeComponent* Shard = MakeShard(Scope.World, Ice, FVector(0, 0, 0), 150.f);

	// THE CASE THAT USED TO FAIL. TrySolidify required one side to literally be an
	// AARPGFluidPool, so a river fell through to an ordinary energy trade and an
	// ice shard into it made ice and no floe.
	TestTrue(TEXT("Ice meeting a river freezes it"), Fluids->TrySolidify(River, Shard));
	TestEqual(TEXT("Producing one floe"), Fluids->GetSolids().Num(), 1);

	// AND THE RIVER DOES NOT RUN OUT, which is what bReservoir means everywhere
	// else in the codebase -- Consume already refuses to spend one, and there is a
	// reaction test named for it. Freezing was the path that forgot.
	TestEqual(TEXT("A river does not run out"), River->GetEnergy(), 10000.f);
	TestEqual(TEXT("And no pool was invented to hold it"), Fluids->GetPools().Num(), 0);

	// Freezing it again still works, because there is still a river there.
	UARPGElementalVolumeComponent* Second = MakeShard(Scope.World, Ice, FVector(600, 0, 0), 150.f);
	TestTrue(TEXT("And can be frozen again"), Fluids->TrySolidify(River, Second));
	TestEqual(TEXT("Making a second floe"), Fluids->GetSolids().Num(), 2);

	// The shard is spent either way -- it is the thing that was used up.
	TestEqual(TEXT("The shard is spent"), Shard->GetEnergy(), 0.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFreezeEdgeTest,
	"ARPG.World.Fluid.Reservoir.AFrozenPatchStopsAtTheBank",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFreezeEdgeTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	Fluids->Solids = { MakeIce(Ice) };
	Fluids->CombinationTable = MakeFreezeTable(Ice);

	// A NARROW river -- 100cm to either side of centre -- and a shard far wider
	// than it. The frozen patch has to stop at the water, not spread over both
	// banks, and the footprint march is what makes that fall out: it asks
	// ContainsPoint outward until the water stops.
	UARPGReservoirVolumeComponent* River =
		MakeRiver(Scope.World, Water, FVector(0, 0, 0), FVector(2000, 100, 350));

	UARPGElementalVolumeComponent* Shard = MakeShard(Scope.World, Ice, FVector(0, 0, 0), 500.f);

	TestTrue(TEXT("A shard across a narrow river still freezes it"),
		Fluids->TrySolidify(River, Shard));

	if (Fluids->GetSolids().Num() != 1)
	{
		AddError(TEXT("Setup: expected exactly one floe."));
		return false;
	}

	AARPGFluidSolid* Floe = Fluids->GetSolids()[0];

	TestTrue(TEXT("You can stand on it mid-river"),
		Floe->IsStandableAt(FVector(0, 0, Floe->GetSurfaceHeight())));

	// The bank is 100 out; the shard reached 500. A patch that ignored containment
	// would happily have frozen dry ground 400cm from the water.
	TestFalse(TEXT("But not out on the bank the shard also covered"),
		Floe->IsStandableAt(FVector(0, 400, Floe->GetSurfaceHeight())));

	// A shard that misses the water entirely freezes nothing at all, which the old
	// bounding-box answer would have got wrong for anything that bends.
	UARPGElementalVolumeComponent* Missed =
		MakeShard(Scope.World, Ice, FVector(0, 1500, 0), 100.f);

	TestFalse(TEXT("A shard that struck the bank freezes nothing"),
		Fluids->TrySolidify(River, Missed));
	TestEqual(TEXT("So there is still one floe"), Fluids->GetSolids().Num(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidReservoirPoolTest,
	"ARPG.World.Fluid.Reservoir.APoolBigEnoughToBeOneIsNotDepleted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidReservoirPoolTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;   // isolate freezing from drying

	// Low enough that the lake below crosses it. bReservoir is a THRESHOLD rather
	// than a flag, so the same asset describes a splash and a lake.
	WaterDefinition->ReservoirArea = 10000.f;

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { MakeIce(Ice) };
	Fluids->CombinationTable = MakeFreezeTable(Ice);

	AARPGFluidPool* Lake = Fluids->Deposit(FVector(0, 0, 0), 400.f, TAG_Element_Water);
	if (!Lake)
	{
		AddError(TEXT("Setup: no lake was deposited."));
		return false;
	}

	TestTrue(TEXT("Setup: it is big enough to be a reservoir"), Lake->Volume->bReservoir);
	const double Before = Lake->GetArea();

	UARPGElementalVolumeComponent* Shard = MakeShard(Scope.World, Ice, FVector(200, 0, 0), 150.f);
	TestTrue(TEXT("A lake freezes"), Fluids->TrySolidify(Lake->Volume, Shard));

	// The same rule, reached the other way: a pool that has gathered enough to be
	// bottomless is bottomless, and freezing it must not eat it. This used to
	// shrink -- and destroy, once enough had been frozen off -- a body it had
	// already agreed could not run out.
	TestEqual(TEXT("A lake is not shrunk by freezing"), Lake->GetArea(), Before, 1.0);
	TestEqual(TEXT("And is still there"), Fluids->GetPools().Num(), 1);

	return true;
}

// ---------------------------------------------------------------------------
// Surface
//
// A body is a polygon, and until this landed that was ALL it was: the fluid
// system had every operation on a shape and no way to see one. The mesh is the
// same ring the simulation uses, so there is no second representation to drift.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidSurfaceTest,
	"ARPG.World.Fluid.Surface.APoolIsDrawnFromItsOwnOutline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidSurfaceTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);

	// A real, always-loaded engine material rather than a transient one, so the
	// soft pointer has something with a stable path to resolve.
	WaterDefinition->SurfaceMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
	Fluids->Definitions = { WaterDefinition };

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 200.f, TAG_Element_Water);
	if (!Pool)
	{
		AddError(TEXT("Setup: no pool was deposited."));
		return false;
	}

	TestTrue(TEXT("A deposited pool has a surface to see"),
		Pool->GetSurfaceTriangleCount() > 0);

	// A SLAB, not a flat cap: the depth is what gives the water an edge. Two caps
	// plus a wall quad per ring segment is strictly more than the cap alone.
	TestTrue(TEXT("And is a slab rather than a flat sheet"),
		Pool->GetSurfaceTriangleCount() > 2 * Pool->GetRing().Num());

	TestEqual(TEXT("Drawn with the material its definition names"),
		Pool->GetSurfaceMaterial(), static_cast<UMaterialInterface*>(UMaterial::GetDefaultMaterial(MD_Surface)));

	// The mesh follows the simulation, because it IS the simulation's ring. Rain
	// is an outward offset, so the outline it produces has to reach the mesh.
	Fluids->bRaining = true;
	Fluids->StepSimulation(1.f);

	TestTrue(TEXT("A grown pool still has a surface"), Pool->GetSurfaceTriangleCount() > 0);

	// Eroded to nothing: the surface has to GO. The subsystem destroys a pool this
	// small, but a client sees the emptied outline replicate first, and a bare
	// early return there leaves a puddle hanging in the air until it catches up.
	Pool->SetRing(TArray<FVector2D>());
	TestEqual(TEXT("An outline with nothing left draws nothing"),
		Pool->GetSurfaceTriangleCount(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidReplicationTest,
	"ARPG.World.Fluid.Surface.TheOutlineIsAllThatTravels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidReplicationTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	// NOTHING SENDS MESH DATA. A client is given the outline, the ground height
	// and the definition, and everything it draws and walks on is rebuilt from
	// those three -- so they are the three that have to carry the Net flag. This
	// asserts the wiring, because the failure mode is silent: the actor still
	// replicates, the client just receives an empty ring and renders nothing.
	for (const TCHAR* Name : { TEXT("Ring"), TEXT("GroundHeight") })
	{
		const FProperty* Property = AARPGFluidBody::StaticClass()->FindPropertyByName(Name);
		TestTrue(FString::Printf(TEXT("A body replicates its %s"), Name),
			Property && Property->HasAnyPropertyFlags(CPF_Net));
	}

	const FProperty* PoolDefinition = AARPGFluidPool::StaticClass()->FindPropertyByName(TEXT("Definition"));
	TestTrue(TEXT("A pool replicates its definition"),
		PoolDefinition && PoolDefinition->HasAnyPropertyFlags(CPF_Net));

	const FProperty* Ice = AARPGFluidSolid::StaticClass()->FindPropertyByName(TEXT("Field"));
	TestTrue(TEXT("A solid replicates its heightfield, holes and all"),
		Ice && Ice->HasAnyPropertyFlags(CPF_Net));

	// And the rebuild genuinely needs nothing else. Setup is server-only, so this
	// assigns exactly what replication would and asks the body to build itself.
	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AARPGFluidPool* Pool = Scope.World->SpawnActor<AARPGFluidPool>(
		AARPGFluidPool::StaticClass(), FTransform::Identity, Params);

	Pool->Definition = MakeWater(GetTransientPackage(), Water);
	Pool->GroundHeight = 250.f;
	Pool->SetRing(ARPGFluidGeometry::MakeCircle(FVector2D(400, 0), 150.0));

	TestTrue(TEXT("A body given only replicated state draws itself"),
		Pool->GetSurfaceTriangleCount() > 0);
	TestEqual(TEXT("At the height it was told"), Pool->GetActorLocation().Z, 250.f, 1.f);
	TestTrue(TEXT("And knows where it is"), Pool->ContainsPoint(FVector(400, 0, 250)));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidSolidSurfaceTest,
	"ARPG.World.Fluid.Surface.AFloeIsWalkedOnWhereItIsDrawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidSolidSurfaceTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	UARPGSolidDefinition* IceDefinition = NewObject<UARPGSolidDefinition>();
	IceDefinition->Element = Ice;
	IceDefinition->Thickness = 30.f;
	IceDefinition->bStandable = true;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AARPGFluidSolid* Floe = Scope.World->SpawnActor<AARPGFluidSolid>(
		AARPGFluidSolid::StaticClass(), FTransform::Identity, Params);

	Floe->Setup(IceDefinition, ARPGFluidGeometry::MakeCircle(FVector2D::ZeroVector, 300.0), 0.f);

	// A gap melted clean through the middle. Not a ring anyone tracks -- just the
	// cells whose top has met their bottom.
	Floe->MeltAt(FVector2D::ZeroVector, 60.f, IceDefinition->Thickness * 4.f);

	TestTrue(TEXT("A floe has a surface"), Floe->GetSurfaceTriangleCount() > 0);

	// THE DRAWN SURFACE IS THE WALKABLE ONE. This used to block pawns with the
	// bounds BOX -- the polygon's rectangle -- so a player could stand off the floe
	// and inside the box, in mid-air over open water. Invisible while nothing was
	// drawn; the first thing you notice once the slab is there.
	TestNotEqual(TEXT("Its mesh carries the collision"),
		Floe->GetSurfaceComponent()->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
	TestEqual(TEXT("And its broadphase box has gone back to being broadphase"),
		Floe->Bounds->GetCollisionResponseToChannel(ECC_Pawn), ECR_Overlap);

	// Which agrees with the gameplay answer, and both know about the hole.
	TestTrue(TEXT("You can stand on the slab"),
		Floe->IsStandableAt(FVector(200, 0, Floe->GetSurfaceHeight())));
	TestFalse(TEXT("But not down the hole melted through it"),
		Floe->IsStandableAt(FVector(0, 0, Floe->GetSurfaceHeight())));

	// A sheet of frost is not something you stand on, and says so the same way.
	IceDefinition->bStandable = false;
	Floe->SetRing(ARPGFluidGeometry::MakeCircle(FVector2D::ZeroVector, 300.0));

	TestEqual(TEXT("An unstandable solid carries no collision at all"),
		Floe->GetSurfaceComponent()->GetCollisionEnabled(), ECollisionEnabled::NoCollision);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidCastOverSlabTest,
	"ARPG.World.Fluid.Casting.ASpellOverASlabWetsWhatTheSlabIsFloatingIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidCastOverSlabTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	MakeGround(Scope.World, 0.f);

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;

	UARPGSolidDefinition* IceDefinition = MakeIce(Ice);
	IceDefinition->Thickness = 40.f;

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { IceDefinition };
	Fluids->CombinationTable = MakeFreezeTable(Ice);

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 600.f, TAG_Element_Water);
	UARPGElementalVolumeComponent* Shard = MakeShard(Scope.World, Ice, FVector(0, 0, 0), 300.f);

	if (!Fluids->TrySolidify(Pool->Volume, Shard) || Fluids->GetSolids().Num() != 1)
	{
		AddError(TEXT("Setup: nothing froze."));
		return false;
	}

	AARPGFluidSolid* Floe = Fluids->GetSolids()[0];
	const int32 PoolsBefore = Fluids->GetPools().Num();

	// A SLAB IS NOT GROUND. It blocks every channel because you stand on it, so
	// the deposit probe used to hit the ICE and leave a puddle on top of the floe
	// -- at the wrong height, and as a separate actor that does NOT drift with the
	// floe, so it hung over open water the moment the floe moved on. What that
	// spell wet is whatever the floe is floating in.
	MakeCastSpell(Scope.World, Water, FVector(0, 0, 400), FVector(0, 0, 400),
		/*Radius=*/100.f)->Destroy();

	TestEqual(TEXT("It merged into the water under the floe"),
		Fluids->GetPools().Num(), PoolsBefore);

	for (const AARPGFluidPool* Wet : Fluids->GetPools())
	{
		TestTrue(TEXT("And no body sits at the height of the ice"),
			Wet->GroundHeight < Floe->GetSurfaceHeight() - 1.f);
	}

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
