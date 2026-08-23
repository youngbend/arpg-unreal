// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "ARPGTestFixtures.h"

#if WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS

#include "ARPGConductionSubsystem.h"
#include "ARPGDischargeContext.h"
#include "ARPGDischargeEffect.h"
#include "ARPGElementPalette.h"
#include "ARPGElementalReactionSubsystem.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGSurfaceBody.h"
#include "ARPGSolidBody.h"
#include "ARPGSlabEffects.h"
#include "ARPGFluidDefinition.h"
#include "ARPGSolidDefinition.h"
#include "ARPGFluidGeometry.h"
#include "ARPGWorld.h"
#include "ARPGFluidSurfaceSubsystem.h"
#include "ARPGGameplayTags.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicElement.h"
#include "ARPGPlaceholderEffect.h"
#include "ARPGReservoirVolumeComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
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
	using ARPGTest::FTestWorld;

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
	 * The Solidify row that makes an agent and water set, and nothing else.
	 *
	 * NOTHING IN C++ KNOWS THAT WATER FREEZES -- this row, Surface-scoped, is the
	 * whole declaration, and it is the same table every other relationship uses.
	 *
	 * THE AGENT'S TAG IS A PARAMETER because not everything that sets a surface is
	 * ice. An earth crust is agent and product both, and a row still naming ice
	 * matches nothing the test then goes on to do.
	 */
	UARPGMagicCombinationTable* MakeFreezeTable(UARPGMagicElement* Ice,
		FGameplayTag AgentTag = TAG_Element_Ice)
	{
		UARPGMagicCombinationTable* Table = NewObject<UARPGMagicCombinationTable>();

		UARPGMagicCombinationEntry* Entry = NewObject<UARPGMagicCombinationEntry>(Table);
		Entry->RequiredElements.AddTag(AgentTag);
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

// ---------------------------------------------------------------------------

namespace ARPGFluidTestUtils
{
	inline UARPGFluidDefinition* MakeLava(UObject* Outer, UARPGMagicElement* Element)
	{
		UARPGFluidDefinition* Definition = NewObject<UARPGFluidDefinition>(Outer);
		Definition->Element = Element;
		Definition->Depth = 35.f;
		Definition->MinimumArea = 2500.f;
		Definition->ReservoirArea = 100000000.f;
		Definition->EnergyPerArea = 0.005f;
		Definition->Conductivity = 0.05f;
		Definition->RainGrowthRate = 0.f;
		Definition->EvaporationRate = 0.f;   // held still, so a test can assert on area
		Definition->MergeDistance = 90.f;
		Definition->Density = 0.0027f;
		// Thick: an eighth of water's rate, and a yield slope that stops it on a
		// gradient water would sheet off.
		Definition->FlowRate = 0.75f;
		Definition->YieldSlope = 2.f;
		Definition->MinimumFilm = 0.4f;
		return Definition;
	}

	inline UARPGSolidDefinition* MakeObsidian(UARPGMagicElement* Element)
	{
		UARPGSolidDefinition* Definition = NewObject<UARPGSolidDefinition>();
		Definition->Element = Element;
		Definition->Thickness = 25.f;
		Definition->bStandable = true;
		Definition->EnergyPerArea = 0.f;   // no spell can break it
		Definition->BreaksInto = nullptr;  // rock that formed on lava is not frozen lava
		Definition->MinimumArea = 100.f;
		Definition->Density = 0.0024f;     // lighter than the lava, so a crust floats
		Definition->SettleSpeed = 1000.f;
		return Definition;
	}

	/** The shipped rows for lava, transcribed. */
	inline UARPGMagicCombinationTable* MakeLavaTable(UARPGMagicElement* Lava,
		UARPGMagicElement* Obsidian, UARPGMagicElement* Steam)
	{
		UARPGMagicCombinationTable* Table = NewObject<UARPGMagicCombinationTable>();

		// Fire + earth, in hand AND in the world. Scope 1|2 and not Field: a grass
		// fire crossing stony ground is a grass fire, not a lava flow.
		UARPGMagicCombinationEntry* Melt = NewObject<UARPGMagicCombinationEntry>(Table);
		Melt->RequiredElements.AddTag(TAG_Element_Fire);
		Melt->RequiredElements.AddTag(TAG_Element_Earth);
		Melt->Result = Lava;
		Melt->Scope = static_cast<int32>(EARPGCombinationScope::Hand)
			| static_cast<int32>(EARPGCombinationScope::Collision);
		Table->Entries.Add(Melt);

		// Water onto a BODY of lava sets its surface. Surface scope only, which is
		// the whole of "obsidian cannot be made in hand".
		UARPGMagicCombinationEntry* Quench = NewObject<UARPGMagicCombinationEntry>(Table);
		Quench->RequiredElements.AddTag(TAG_Element_Water);
		Quench->RequiredElements.AddTag(TAG_Element_Lava);
		Quench->Result = Obsidian;
		Quench->Mode = EARPGReactionMode::Solidify;
		Quench->Scope = static_cast<int32>(EARPGCombinationScope::Surface);
		Table->Entries.Add(Quench);

		// And the same pair meeting in the AIR is steam, which is why scope exists.
		UARPGMagicCombinationEntry* Air = NewObject<UARPGMagicCombinationEntry>(Table);
		Air->RequiredElements.AddTag(TAG_Element_Water);
		Air->RequiredElements.AddTag(TAG_Element_Lava);
		Air->Result = Steam;
		Air->Scope = static_cast<int32>(EARPGCombinationScope::Collision);
		Table->Entries.Add(Air);

		return Table;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGLavaRecipeTest,
	"ARPG.World.Fluid.Lava.FireAndEarthMakeLavaInHandAndInTheWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGLavaRecipeTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;

	UARPGMagicElement* Lava = MakeElement(GetTransientPackage(), TAG_Element_Lava);
	UARPGMagicElement* Obsidian = MakeElement(GetTransientPackage(), TAG_Element_Obsidian);
	UARPGMagicElement* Steam = MakeElement(GetTransientPackage(), TAG_Element_Steam);

	UARPGMagicCombinationTable* Table = MakeLavaTable(Lava, Obsidian, Steam);

	FGameplayTagContainer FireEarth;
	FireEarth.AddTag(TAG_Element_Fire);
	FireEarth.AddTag(TAG_Element_Earth);

	// THE UNUSUAL PART. Most pairs mean different things in a caster's hands and
	// out in the world, which is the entire reason scope exists -- and this one
	// does not. Fusing fire and earth is molten rock; throwing fire at rock is
	// molten rock.
	const UARPGMagicCombinationEntry* InHand =
		Table->ResolveEntry(FireEarth, EARPGCombinationScope::Hand);
	const UARPGMagicCombinationEntry* InWorld =
		Table->ResolveEntry(FireEarth, EARPGCombinationScope::Collision);

	TestNotNull(TEXT("Fire and earth combine in hand"), InHand);
	TestNotNull(TEXT("And on collision"), InWorld);

	if (InHand && InWorld)
	{
		TestSamePtr(TEXT("Into lava, held"), InHand->Result.Get(), Lava);
		TestSamePtr(TEXT("And lava, thrown"), InWorld->Result.Get(), Lava);
		TestSamePtr(TEXT("The same row answering both"), InHand, InWorld);
	}

	// NOT IN A FIELD, though. Fire spreading across stony ground is a grass fire,
	// not a lava flow, and the spread solver asks in its own scope.
	TestNull(TEXT("But fire over stony ground is not a lava flow"),
		Table->ResolveEntry(FireEarth, EARPGCombinationScope::Field));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGObsidianNotInHandTest,
	"ARPG.World.Fluid.Lava.ObsidianCannotBeMadeInHand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGObsidianNotInHandTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;

	UARPGMagicElement* Lava = MakeElement(GetTransientPackage(), TAG_Element_Lava);
	UARPGMagicElement* Obsidian = MakeElement(GetTransientPackage(), TAG_Element_Obsidian);
	UARPGMagicElement* Steam = MakeElement(GetTransientPackage(), TAG_Element_Steam);

	UARPGMagicCombinationTable* Table = MakeLavaTable(Lava, Obsidian, Steam);

	FGameplayTagContainer WaterLava;
	WaterLava.AddTag(TAG_Element_Water);
	WaterLava.AddTag(TAG_Element_Lava);

	// THERE IS NO MECHANISM THAT FORBIDS HOLDING AN ELEMENT, and there does not
	// need to be. What an element can BE in a caster's hands is exactly what some
	// row produces in the Hand scope, so scoping the Quench row to Surface is the
	// whole of the restriction -- no flag, no list, nothing to keep in sync.
	TestNull(TEXT("Water and lava fuse into nothing in the hand"),
		Table->ResolveEntry(WaterLava, EARPGCombinationScope::Hand));

	// Nor anywhere else, other than the two places it means something.
	for (const UARPGMagicCombinationEntry* Entry : Table->Entries)
	{
		if (Entry && Entry->Result == Obsidian)
		{
			TestFalse(TEXT("No row anywhere produces obsidian in hand"),
				Entry->AppliesTo(EARPGCombinationScope::Hand));
			TestTrue(TEXT("Only on the surface of a body of it"),
				Entry->AppliesTo(EARPGCombinationScope::Surface));
		}
	}

	// AND THE PAIR STILL MEANS SOMETHING IN THE AIR. A Surface-only row would
	// otherwise leave a water jet crossing a lava jet to merely neutralise, which
	// is the quiet hole scoping one narrowly can leave behind.
	const UARPGMagicCombinationEntry* Air =
		Table->ResolveEntry(WaterLava, EARPGCombinationScope::Collision);

	TestNotNull(TEXT("Water meeting lava mid-air still does something"), Air);
	if (Air)
	{
		TestSamePtr(TEXT("Which is steam, not glass"), Air->Result.Get(), Steam);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGObsidianCrustTest,
	"ARPG.World.Fluid.Lava.WaterSetsTheSurfaceOfAFlowToGlass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGObsidianCrustTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Lava = MakeElement(GetTransientPackage(), TAG_Element_Lava);
	UARPGMagicElement* Obsidian = MakeElement(GetTransientPackage(), TAG_Element_Obsidian);
	UARPGMagicElement* Steam = MakeElement(GetTransientPackage(), TAG_Element_Steam);

	UARPGFluidDefinition* LavaDefinition = MakeLava(GetTransientPackage(), Lava);
	UARPGSolidDefinition* ObsidianDefinition = MakeObsidian(Obsidian);

	Fluids->Definitions = { LavaDefinition };
	Fluids->Solids = { ObsidianDefinition };
	Fluids->CombinationTable = MakeLavaTable(Lava, Obsidian, Steam);

	AARPGFluidPool* Flow = Fluids->Deposit(FVector(0, 0, 0), 400.f, TAG_Element_Lava);
	if (!Flow)
	{
		AddError(TEXT("Setup: nothing pooled."));
		return false;
	}

	const float Surface = Flow->GetSurfaceHeight();

	UARPGElementalVolumeComponent* Jet = MakeShard(Scope.World, Water, FVector(0, 0, 0), 200.f);

	// NOTHING IN C++ KNOWS THAT WATER QUENCHES LAVA. It is a Solidify row in the
	// Surface scope -- the same table, the same scoping and the same code path
	// that turns an ice shard on a puddle into a floe.
	TestTrue(TEXT("Water on a flow sets it"), Fluids->TrySolidify(Flow->Volume, Jet));
	TestEqual(TEXT("Producing one crust"), Fluids->GetSolids().Num(), 1);

	if (Fluids->GetSolids().Num() != 1)
	{
		return false;
	}

	AARPGSolidBody* Crust = Fluids->GetSolids()[0];

	// PERMANENT BOTH WAYS, which is the pair of independent questions the solid
	// definition exists to keep apart. Volcanic glass is the one thing here that,
	// once made, is simply part of the level.
	TestTrue(TEXT("Obsidian is permanent"), Crust->IsPermanent());
	TestEqual(TEXT("And worth no energy to a spell"),
		Crust->GetSurfaceEnergyDensity(), 0.f);

	// So a fireball does nothing to it. Asked THROUGH THE VOLUME, which is the
	// path a reaction actually takes: OnElementalReaction bails on a zero density
	// before it converts anything into ground, which is cheap as well as
	// absolute. Calling ConsumeSurfaceArea directly would walk straight past the
	// guard being asserted and melt a bowl out of permanent rock.
	const double Before = Crust->GetArea();
	Crust->Volume->Consume(500.f, nullptr);

	TestEqual(TEXT("Nothing takes a bite out of it"), Crust->GetArea(), Before, 1.0);

	// A CRUST, NOT A RAFT. Lighter than the lava, so it floats -- barely. At 89%
	// of the flow's density a 25cm slab rides 22cm under, scabbing the surface,
	// nothing like the freeboard ice gets. Same Archimedes, different two numbers.
	Crust->Tick(1.f);

	TestEqual(TEXT("It rides almost flush with the flow"), Crust->Draft, 22.2f, 1.5f);
	TestTrue(TEXT("Just proud of it"), Crust->GetSurfaceHeight() > Surface);
	TestTrue(TEXT("But barely"), Crust->GetSurfaceHeight() < Surface + 6.f);

	// AND IT MELTS INTO NOTHING. Rock that formed on lava is not frozen lava:
	// break it and you get rubble, not a flow back into the pool.
	TestNull(TEXT("It gives nothing back"), ObsidianDefinition->BreaksInto.Get());

	return true;
}

// ---------------------------------------------------------------------------

namespace ARPGFluidTestUtils
{
	/** Earth: rooted, permanent, and never frozen out of anything. */
	inline UARPGSolidDefinition* MakeEarth(UARPGMagicElement* Element)
	{
		UARPGSolidDefinition* Definition = NewObject<UARPGSolidDefinition>();
		Definition->Element = Element;
		Definition->Thickness = 120.f;
		Definition->bStandable = true;
		Definition->BreaksInto = nullptr; // it was never a liquid
		Definition->MinimumArea = 100.f;
		Definition->EnergyPerArea = 0.02f;
		Definition->Density = 0.0025f;
		Definition->RiseSpeed = 14.f;
		return Definition;
	}

	inline AARPGSolidBody* RaiseSlab(UWorld* World, UARPGFluidSurfaceSubsystem* Fluids,
		UARPGSolidDefinition* Definition, const FVector2D& At, double Radius)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AARPGSolidBody* Slab = World->SpawnActor<AARPGSolidBody>(
			AARPGSolidBody::StaticClass(), FTransform::Identity, Params);

		Slab->Setup(Definition, ARPGFluidGeometry::MakeCircle(At, Radius), 0.f);
		Fluids->RegisterSolid(Slab);
		return Slab;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSlabRaisedTest,
	"ARPG.World.Fluid.Slabs.ARaisedSlabIsRootedPermanentAndKnown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSlabRaisedTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Earth = MakeElement(GetTransientPackage(), TAG_Element_Earth);
	UARPGSolidDefinition* EarthDefinition = MakeEarth(Earth);
	Fluids->Solids = { EarthDefinition };

	AARPGSolidBody* Slab = RaiseSlab(Scope.World, Fluids, EarthDefinition,
		FVector2D::ZeroVector, 200.0);

	// REGISTERED BY HAND, because nothing solidified it. Unregistered it would
	// still draw, collide and react -- but a bolt striking water it stands in
	// would conduct as though the wall were not there.
	TestEqual(TEXT("The world knows about a slab it did not freeze"),
		Fluids->GetSolids().Num(), 1);
	TestTrue(TEXT("And it roofs over the ground it stands on"),
		Fluids->IsCoveredBySolid(FVector(0, 0, 0)));
	TestSamePtr(TEXT("And can be found, not just counted"),
		Fluids->FindSolidAt(FVector(0, 0, 0)), Slab);

	// PERMANENT, because nothing takes a slab with time -- the same fact the weather
	// tick skip it. Two questions, one answer, rather than a second flag that can
	// disagree with the first.
	TestTrue(TEXT("It is permanent"), Slab->IsPermanent());

	const double Before = Slab->GetArea();
	for (int32 Tick = 0; Tick < 20; ++Tick)
	{
		Fluids->StepSimulation(0.25f);
	}

	TestEqual(TEXT("Five seconds of weather does not touch it"), Slab->GetArea(), Before, 1.0);
	TestEqual(TEXT("And it is still there"), Fluids->GetSolids().Num(), 1);

	// ROOTED. No FloatsOn, so the buoyancy path never runs: it does not settle to
	// a waterline, does not drift, and does not ask a surface that is not there
	// for a density.
	TestNull(TEXT("It floats on nothing"), Slab->FloatsOn.GetObject());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSlabFrameTest,
	"ARPG.World.Fluid.Slabs.ASlabCanTurnWithoutItsCellsMoving",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSlabFrameTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Earth = MakeElement(GetTransientPackage(), TAG_Element_Earth);
	UARPGSolidDefinition* EarthDefinition = MakeEarth(Earth);
	Fluids->Solids = { EarthDefinition };

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AARPGSolidBody* Bar = Scope.World->SpawnActor<AARPGSolidBody>(
		AARPGSolidBody::StaticClass(), FTransform::Identity, Params);

	// A LONG BAR, because a disc would pass a rotation test by symmetry and tell
	// you nothing. This one runs east-west and is narrow north-south.
	Bar->Setup(EarthDefinition,
		ARPGFluidGeometry::MakeStadium(FVector2D(-400, 0), FVector2D(400, 0), 60.0), 0.f);

	TestTrue(TEXT("Standable along its length"), Bar->IsStandableAt(FVector(300, 0, 0)));
	TestFalse(TEXT("And not across it"), Bar->IsStandableAt(FVector(0, 300, 0)));

	// THE FRAME STARTS AT THE OUTLINE'S OWN CENTRE, which is what Setup does with
	// a ring clipped in world coordinates: recentre it and remember where the
	// centre was.
	TestEqual(TEXT("The frame starts unturned"), Bar->SlabYaw, 0.f);

	// A quarter turn. The polygon does not move; the frame does.
	const int32 VertsBefore = Bar->GetOutline().Num();
	const double AreaBefore = Bar->GetArea();

	Bar->SlabYaw = 90.f;

	TestEqual(TEXT("Turning touches no vertices"), Bar->GetOutline().Num(), VertsBefore);
	TestEqual(TEXT("Nor the area it keeps in its own frame"), Bar->GetArea(), AreaBefore, 1.0);

	// AND THE WORLD SEES IT TURNED. This is the whole point: the shape is held in
	// the slab's own frame, so moving and turning it is a transform on the actor
	// and the outline never learns anything happened. Yaw only -- a floe spins on
	// the water, it does not tumble.
	TestFalse(TEXT("No longer standable where it used to run"),
		Bar->IsStandableAt(FVector(300, 0, 0)));
	TestTrue(TEXT("And standable across where it did not"),
		Bar->IsStandableAt(FVector(0, 300, 0)));

	// The mapping is invertible, which everything above quietly depends on.
	const FVector2D Probe(137.f, -84.f);
	const FVector2D RoundTrip = Bar->ToLocal(Bar->ToWorld(Probe));

	TestEqual(TEXT("World and local round-trip"), RoundTrip.X, Probe.X, 0.01);
	TestEqual(TEXT("In both axes"), RoundTrip.Y, Probe.Y, 0.01);

	// AND MOVING IS THE FRAME TOO, so a drifting floe costs a transform rather
	// than a rebuilt polygon.
	Bar->SlabYaw = 0.f;
	Bar->SlabOrigin = FVector2D(1000, 0);

	TestFalse(TEXT("Moved out from under where it was"),
		Bar->IsStandableAt(FVector(300, 0, 0)));
	TestTrue(TEXT("And onto where it went"), Bar->IsStandableAt(FVector(1300, 0, 0)));
	TestEqual(TEXT("Still without touching a vertex"),
		Bar->GetOutline().Num(), VertsBefore);

	// What the world thinks the slab's centre is follows the frame.
	TestEqual(TEXT("Its world centre moved with it"),
		Bar->GetWorldCentre().X, 1000.0, 1.0);

	// And the reach query the launch spell uses reads the same frame.
	TestEqual(TEXT("A point inside it is no distance away"),
		Bar->DistanceToEdge(FVector2D(1300, 0)), 0.0, 1.0);
	TestTrue(TEXT("And one well clear of it is"),
		Bar->DistanceToEdge(FVector2D(1000, 900)) > 500.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSlabRiseTest,
	"ARPG.World.Fluid.Slabs.ARaisedSlabClimbsOutOfTheGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSlabRiseTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Earth = MakeElement(GetTransientPackage(), TAG_Element_Earth);
	UARPGSolidDefinition* EarthDefinition = MakeEarth(Earth);
	Fluids->Solids = { EarthDefinition };

	AARPGSolidBody* Slab = RaiseSlab(Scope.World, Fluids, EarthDefinition,
		FVector2D::ZeroVector, 200.0);

	// BURIED TO ITS OWN THICKNESS, which is what makes it climb out rather than
	// appear. Draft already means "how far under its resting height this is
	// sitting", so a spell that raises a slab sets it and the rise IS the number
	// coming back to zero -- no second concept, no animation track.
	const double Resting = Slab->GetActorLocation().Z;

	Slab->BeginBuried(EarthDefinition->Thickness);
	TestEqual(TEXT("It starts buried"), Slab->Draft, EarthDefinition->Thickness, 0.01f);

	// AND IS ACTUALLY DOWN THERE. Recording the depth without moving the slab
	// would leave it standing in full view until the first tick dropped it -- a
	// wall that appears, sinks, then rises, which is worse than not animating.
	const double Buried = Slab->GetActorLocation().Z;
	TestTrue(TEXT("And is below where it will end up"), Buried < Resting);

	for (int32 Tick = 0; Tick < 120 && Slab->Draft > 0.f; ++Tick)
	{
		Slab->Tick(1.f / 60.f);
	}

	// SNAPPED HOME rather than approached forever. FInterpTo is asymptotic, so a
	// slab a fraction of a millimetre short would tick, move and dirty its
	// replicated draft for the rest of the level's life.
	TestEqual(TEXT("Two seconds later it is all the way out"), Slab->Draft, 0.f);
	TestTrue(TEXT("And it ended up higher than it started"),
		Slab->GetActorLocation().Z > Buried);

	TestEqual(TEXT("Back at the height it was built for"),
		Slab->GetActorLocation().Z, Resting, 0.01);

	// And having arrived, it stays: nothing moves a rooted slab again.
	Slab->Tick(1.f);
	TestEqual(TEXT("Then it never moves again"),
		Slab->GetActorLocation().Z, Resting, 0.01);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGThrownSlabTest,
	"ARPG.World.Fluid.Slabs.AThrownSlabLandsAsTheSlabThatWasThrown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGThrownSlabTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Earth = MakeElement(GetTransientPackage(), TAG_Element_Earth);
	UARPGSolidDefinition* EarthDefinition = MakeEarth(Earth);
	Fluids->Solids = { EarthDefinition };

	AARPGSolidBody* Pillar = RaiseSlab(Scope.World, Fluids, EarthDefinition,
		FVector2D::ZeroVector, 200.0);

	// BROKEN FIRST, because that is the whole point of carrying the outline rather
	// than a size: a pillar a spell has already chipped should come down chipped.
	Pillar->ConsumeSurfaceArea(Pillar->GetArea() * 0.25);

	const TArray<FVector2D> Before = Pillar->GetOutline();
	const int32 VertsBefore = Before.Num();
	const double AreaBefore = Pillar->GetArea();

	TestTrue(TEXT("Setup: the pillar has an outline"), VertsBefore >= 3);

	// Stand in for the launch: the projectile picks the outline up whole.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AARPGLaunchSlabProjectile* Throw = Scope.World->SpawnActor<AARPGLaunchSlabProjectile>(
		AARPGLaunchSlabProjectile::StaticClass(), FTransform::Identity, Params);

	Throw->Carried = Before;
	Throw->CarriedHole = Pillar->GetHole();
	Throw->CarriedDefinition = EarthDefinition;

	Fluids->UnregisterSolid(Pillar);
	Pillar->Destroy();

	TestEqual(TEXT("Nothing is standing while it is in the air"),
		Fluids->GetSolids().Num(), 0);

	// AND IT COMES BACK DOWN. Somewhere else, upright, and carrying every mark it
	// had when it was picked up.
	AARPGSolidBody* Landed = Throw->PutDown(FVector(2000, 500, 0));

	TestNotNull(TEXT("A thrown slab lands as a slab"), Landed);
	if (!Landed)
	{
		return false;
	}

	TestEqual(TEXT("Back on the register"), Fluids->GetSolids().Num(), 1);
	TestEqual(TEXT("With the outline it was carrying"),
		Landed->GetOutline().Num(), VertsBefore);
	TestEqual(TEXT("And the ground it covered"), Landed->GetArea(), AreaBefore, 1.0);

	// WHERE IT WAS PUT, which is the frame doing the placing rather than the
	// polygon moving.
	TestEqual(TEXT("At the place it came to rest"),
		Landed->GetWorldCentre().X, 2000.0, 5.0);
	TestEqual(TEXT("In both axes"), Landed->GetWorldCentre().Y, 500.0, 5.0);
	TestTrue(TEXT("And standable there"), Landed->IsStandableAt(FVector(2000, 500, 0)));

	// PUT DOWN ONCE. A projectile ends by hitting something or by expiring and
	// both run the same path, so without the guard a throw could drop two walls.
	TestNull(TEXT("And only once"), Throw->PutDown(FVector(3000, 0, 0)));
	TestEqual(TEXT("However often it is asked"), Fluids->GetSolids().Num(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSlabBudgetTest,
	"ARPG.World.Fluid.Slabs.TheBudgetWillNotCullWhatSomeoneBuilt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSlabBudgetTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Earth = MakeElement(GetTransientPackage(), TAG_Element_Earth);
	UARPGSolidDefinition* EarthDefinition = MakeEarth(Earth);
	Fluids->Solids = { EarthDefinition };
	Fluids->MaxBodiesOfEachKind = 2;

	for (int32 Index = 0; Index < 5; ++Index)
	{
		RaiseSlab(Scope.World, Fluids, EarthDefinition,
			FVector2D(Index * 1000, 0), 150.0 + Index * 10.0);
	}

	Fluids->StepSimulation(0.1f);

	// THE BUDGET IS FOR LITTER. Puddles left crossing a field and floes that were
	// going to melt anyway are unnoticeable to lose. A wall someone raised for
	// cover is the opposite: it is there on purpose, nothing was ever going to
	// remove it, and having it vanish mid-fight because the level accumulated
	// puddles elsewhere is the worst thing this economy could do.
	TestEqual(TEXT("Every permanent slab survives its own budget"),
		Fluids->GetSolids().Num(), 5);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSlabFindNearTest,
	"ARPG.World.Fluid.Slabs.ASpellCanFindTheSlabInFrontOfIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSlabFindNearTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Earth = MakeElement(GetTransientPackage(), TAG_Element_Earth);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	UARPGSolidDefinition* EarthDefinition = MakeEarth(Earth);
	UARPGSolidDefinition* IceDefinition = MakeIce(Ice);
	Fluids->Solids = { EarthDefinition, IceDefinition };

	AARPGSolidBody* Wall = RaiseSlab(Scope.World, Fluids, EarthDefinition,
		FVector2D(400, 0), 150.0);

	// TO THE SLAB, NOT TO ITS ORIGIN. A wall is long, and a caster at one end of
	// one is not far from it -- measuring to the actor would say they were,
	// because a slab's origin is its centroid.
	TestSamePtr(TEXT("A caster beside it finds it"),
		Fluids->FindSolidNear(FVector(300, 0, 0), 400.f, TAG_Element_Earth), Wall);

	TestNull(TEXT("One across the room does not"),
		Fluids->FindSolidNear(FVector(6000, 0, 0), 400.f, TAG_Element_Earth));

	// OF MY OWN ELEMENT. An earth spell throws earth; a wall of ice in front of
	// the caster is somebody else's cover, not this spell's ammunition.
	TestNull(TEXT("And an earth spell will not throw ice"),
		Fluids->FindSolidNear(FVector(300, 0, 0), 400.f, TAG_Element_Ice));

	// An empty tag is the honest "is there a slab here at all".
	TestSamePtr(TEXT("Asking for anything finds it regardless"),
		Fluids->FindSolidNear(FVector(300, 0, 0), 400.f, FGameplayTag()), Wall);

	return true;
}

// ---------------------------------------------------------------------------
// The slab as a solid
//
// A floe is an outline with a thickness -- see AARPGSolidBody.
// Everything interesting that happens to ice happens in the third dimension: a
// bowl melted at an angle into one edge, a step where new ice formed at the
// waterline a load had pushed the surface down to, a hole where the two faces
// met. None of it can be said with a polygon.
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

	AARPGSolidBody* Floe = Fluids->GetSolids()[0];

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
	// RE-SEEDED, not re-outlined. A slab's shape is its cells, and SetRing writes
	// the base class's polygon -- which a solid draws nothing from.
	const double Before = Floe->GetArea();
	Floe->Setup(IceDefinition, ARPGFluidGeometry::MakeCircle(FVector2D::ZeroVector, 300.0),
		Floe->GroundHeight);
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

	AARPGSolidBody* Floe = Fluids->GetSolids()[0];
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

	AARPGSolidBody* Raft = Fluids->GetSolids()[0];

	TestFalse(TEXT("A floe with open water around it is not anchored"), Raft->bAnchored);

	// THE FRAME, not a ring: a slab is its cells, and drifting moves the frame
	// they are read through rather than the cells themselves.
	const double StartX = Raft->GetWorldCentre().X;
	Raft->Tick(1.f);
	const double DriftedX = Raft->GetWorldCentre().X;

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

	AARPGSolidBody* Plug = Fluids->GetSolids()[1];

	TestTrue(TEXT("A floe spanning the water is anchored by the shore"), Plug->bAnchored);

	const double PlugX = Plug->GetWorldCentre().X;
	Plug->Tick(1.f);

	TestEqual(TEXT("And the current cannot move it"),
		Plug->GetWorldCentre().X, PlugX, 0.01);
	TestEqual(TEXT("Nor does it report a velocity to carry anyone"),
		Plug->GetSurfaceComponent()->GetComponentVelocity().X, 0.0);

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
	Crust->EnergyPerArea = 0.f;

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { Crust };
	Fluids->CombinationTable = MakeFreezeTable(Earth, TAG_Element_Earth);

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 400.f, TAG_Element_Water);
	const double Bed = Pool->GroundHeight;

	UARPGElementalVolumeComponent* Agent = MakeShard(Scope.World, Earth, FVector(0, 0, 0), 200.f);
	if (!Fluids->TrySolidify(Pool->Volume, Agent) || Fluids->GetSolids().Num() != 1)
	{
		AddError(TEXT("Setup: nothing solidified."));
		return false;
	}

	AARPGSolidBody* Slab = Fluids->GetSolids()[0];
	Slab->Tick(1.f);

	TestTrue(TEXT("A slab denser than its fluid does not float"), Slab->bAground);

	// It comes to rest ON THE BOTTOM rather than sitting awash at the surface,
	// which is what clamping the draft to the thickness would have given.
	TestEqual(TEXT("It settles onto the bed"),
		Slab->GetActorLocation().Z, Bed, 1.0);

	// And nothing breaks it. EnergyPerArea is the
	// questions -- does time take it, can a reaction take it -- and permanent rock
	// answers no to both.
	const double Before = Slab->GetArea();
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

	// Three puddles in a row, each near enough to the next to carry a charge into
	// it. Nothing authors the connection -- being close IS the connection.
	//
	// SET DIAGONALLY, AND THAT IS NOT COSMETIC. This chain used to be collinear
	// and spaced so the discs plainly OVERLAPPED, which only stayed three bodies
	// because merging was decided by whether one centroid fell inside another's
	// bounding box -- and at this spacing it did not. Two overlapping puddles of
	// the same water are one puddle, and now that they merge, a chain has to be
	// built out of puddles that genuinely do not overlap.
	//
	// Offset on both axes because conduction connects through the volumes' BOXES
	// while merging asks about the outlines: for two circles side by side those
	// are the same question, and on a diagonal they are not. 212 apart with a
	// radius of 100 each is a gap between the discs and an overlap between the
	// squares around them.
	AARPGFluidPool* Near = Fluids->Deposit(FVector(0, 0, 0), 100.f, TAG_Element_Water);
	AARPGFluidPool* Middle = Fluids->Deposit(FVector(150, 150, 0), 100.f, TAG_Element_Water);
	AARPGFluidPool* Far = Fluids->Deposit(FVector(300, 300, 0), 100.f, TAG_Element_Water);

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
	// one two hops away -- which it can only do by hopping, since the far puddle
	// is four metres from the near one and nothing reaches that directly.
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

	AARPGSolidBody* Floe = Fluids->GetSolids()[0];

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
		const FProperty* Property = AARPGSurfaceBody::StaticClass()->FindPropertyByName(Name);
		TestTrue(FString::Printf(TEXT("A body replicates its %s"), Name),
			Property && Property->HasAnyPropertyFlags(CPF_Net));
	}

	const FProperty* PoolDefinition = AARPGFluidPool::StaticClass()->FindPropertyByName(TEXT("Definition"));
	TestTrue(TEXT("A pool replicates its definition"),
		PoolDefinition && PoolDefinition->HasAnyPropertyFlags(CPF_Net));

	// A SOLID NEEDS THREE MORE, and between them they are still a few dozen bytes.
	// This used to be a heightfield -- thousands of cells, run-length encoded and
	// still the heaviest payload in the game, resent every time any of it melted.
	// What a client needs now is the shape, the gap through it, and where that
	// shape is standing.
	for (const TCHAR* Name : { TEXT("Hole"), TEXT("SlabOrigin"), TEXT("SlabYaw") })
	{
		const FProperty* Property = AARPGSolidBody::StaticClass()->FindPropertyByName(Name);
		TestTrue(FString::Printf(TEXT("A solid replicates its %s"), Name),
			Property && Property->HasAnyPropertyFlags(CPF_Net));
	}

	const FProperty* SolidDefinition =
		AARPGSolidBody::StaticClass()->FindPropertyByName(TEXT("Definition"));
	TestTrue(TEXT("And its definition, which is what says how thick it is"),
		SolidDefinition && SolidDefinition->HasAnyPropertyFlags(CPF_Net));

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
	TestEqual(TEXT("At the height it was told"), Pool->GetActorLocation().Z, 250.0, 1.0);
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
	AARPGSolidBody* Floe = Scope.World->SpawnActor<AARPGSolidBody>(
		AARPGSolidBody::StaticClass(), FTransform::Identity, Params);

	// WITH A GAP THROUGH THE MIDDLE, which is what a slab froze around a rock or an
	// island has. A solid KEEPS its hole where a pool does not: a liquid flows back
	// over one, and a floe with a gap you can fall through is a floe with a gap.
	Floe->Setup(IceDefinition, ARPGFluidGeometry::MakeCircle(FVector2D::ZeroVector, 300.0), 0.f,
		ARPGFluidGeometry::MakeCircle(FVector2D::ZeroVector, 90.0));

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
	TestFalse(TEXT("But not down the hole through it"),
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

	AARPGSolidBody* Floe = Fluids->GetSolids()[0];
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

	AARPGSolidBody* Floe = Fluids->GetSolids()[0];

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidSolidifyThroughReactionTest,
	"ARPG.World.Fluid.Solidify.AnIceEmanationOverAPuddleFreezesIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidSolidifyThroughReactionTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;

	// BEGUN PLAY, unlike the rest of this suite. A discharge arms its reaction
	// volume in BeginPlay -- see AARPGDischargeEffect::ConfigureReactionVolume --
	// and the default fixture deliberately does not run it, so the emanation
	// would spawn with a switched-off collider and no energy and meet nothing.
	// The fixture's own comment names this exact case.
	ARPGTest::FTestWorldBegunPlay Scope;

	// THE WHOLE PATH, which nothing covered. FreezesTheOverlapIntoAStandableSlab
	// calls TrySolidify directly, so every step BEFORE it -- the colliders finding
	// each other, OnVolumesMet firing, and the reaction solver deciding this pair
	// is a solidify rather than an energy trade -- was untested. A spell cast in
	// the game goes through all of it.
	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	UARPGElementalReactionSubsystem* Reactions =
		Scope.World->GetSubsystem<UARPGElementalReactionSubsystem>();

	TestNotNull(TEXT("Setup: the reaction subsystem exists"), Reactions);
	if (!Reactions)
	{
		return false;
	}

	MakeGround(Scope.World, 0.f);

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;

	UARPGSolidDefinition* IceDefinition = NewObject<UARPGSolidDefinition>();
	IceDefinition->Element = Ice;
	IceDefinition->Thickness = 30.f;
	IceDefinition->bStandable = true;
	IceDefinition->MinimumArea = 2500.f; // the shipped number, not a lenient one

	// BOTH ROWS THE SHIPPED TABLE HAS FOR THIS PAIR, because having only the
	// Surface one is not the configuration the game runs. DA_MagicCombinations
	// carries Freeze (Collision|Field) alongside FreezeSurface (Surface), and the
	// solver reads the Collision scope on its way to the hand-off.
	UARPGMagicCombinationTable* Table = NewObject<UARPGMagicCombinationTable>();

	UARPGMagicCombinationEntry* Surface = NewObject<UARPGMagicCombinationEntry>(Table);
	Surface->RequiredElements.AddTag(TAG_Element_Ice);
	Surface->RequiredElements.AddTag(TAG_Element_Water);
	Surface->Result = Ice;
	Surface->Mode = EARPGReactionMode::Solidify;
	Surface->Scope = static_cast<int32>(EARPGCombinationScope::Surface);
	Table->Entries.Add(Surface);

	UARPGMagicCombinationEntry* Collision = NewObject<UARPGMagicCombinationEntry>(Table);
	Collision->RequiredElements.AddTag(TAG_Element_Ice);
	Collision->RequiredElements.AddTag(TAG_Element_Water);
	Collision->Result = Ice;
	Collision->Mode = EARPGReactionMode::Auto;
	Collision->Scope = static_cast<int32>(EARPGCombinationScope::Collision)
		| static_cast<int32>(EARPGCombinationScope::Field);
	Table->Entries.Add(Collision);

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { IceDefinition };
	Fluids->CombinationTable = Table;
	Reactions->CombinationTable = Table;

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 300.f, TAG_Element_Water);
	TestNotNull(TEXT("Setup: a puddle exists"), Pool);
	if (!Pool)
	{
		return false;
	}

	// AN EMANATION STANDING OVER IT, spawned exactly as the discharge ability
	// spawns one: deferred, initialised from a context, then finished.
	FARPGDischargeContext Context;
	Context.DischargeType = EARPGDischargeType::Emanate;
	Context.PrimaryElement = Ice;
	Context.Origin = FVector(0, 0, 90);
	Context.Direction = FVector::ForwardVector;
	Context.ComputedDamage = 40.f;
	Context.MinPower = 0.f;
	Context.MaxPower = 1.f;
	Context.PowerFraction = 1.f;

	const FTransform Where(Context.Direction.Rotation(), Context.Origin);

	AARPGPlaceholderDischarge* Emanation = Scope.World->SpawnActorDeferred<AARPGPlaceholderDischarge>(
		AARPGPlaceholderDischarge::StaticClass(), Where, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);

	TestNotNull(TEXT("Setup: the emanation spawns"), Emanation);
	if (!Emanation)
	{
		return false;
	}

	Emanation->InitializeFromContext(Context);
	Emanation->FinishSpawning(Where);

	// The overlap fires on registration, so by here the reaction has either
	// happened or been refused. Ticked once regardless, because an overlap
	// queued during spawn is dispatched on the next update.
	Scope.World->Tick(LEVELTICK_All, 0.1f);

	TestEqual(TEXT("The overlap froze the water under it"), Fluids->GetSolids().Num(), 1);

	if (Fluids->GetSolids().Num() == 0)
	{
		return false;
	}

	const AARPGSolidBody* Floe = Fluids->GetSolids()[0];
	TestTrue(TEXT("Into a real slab"), Floe->GetArea() > 0.0);
	TestTrue(TEXT("You can stand on"),
		Floe->IsStandableAt(FVector(0, 0, Floe->GetSurfaceHeight())));

	// AND THE PUDDLE IS GONE, which is the half that used to take the ice with
	// it. An emanation is metres across and a puddle is not, so the frozen region
	// is the whole pool -- ConsumeSurfaceArea finishes it, the pool retires, and
	// retiring a pool drops everything floating on it. The slab was floating on
	// it, because it had just been made from it.
	TestEqual(TEXT("Having used the whole puddle up"), Fluids->GetPools().Num(), 0);

	// ROOTED, NOT RIDING. What a slab that consumed its own pool is standing on
	// is the bed, and a null FloatsOn is how this codebase says so -- it is also
	// what keeps DropRiders from recognising it as a rider of the pool that is
	// about to go.
	TestNull(TEXT("The ice is rooted rather than floating"), Floe->FloatsOn.GetObject());
	TestTrue(TEXT("And says it is aground"), Floe->bAground);

	// It has to SURVIVE, not merely exist for the frame it was made in: the
	// buoyancy tick reads FloatsOn every frame and a slab that thinks it is riding
	// a destroyed pool is the other way this goes wrong.
	Scope.World->Tick(LEVELTICK_All, 0.1f);
	Scope.World->Tick(LEVELTICK_All, 0.1f);

	TestEqual(TEXT("And is still there a few frames later"), Fluids->GetSolids().Num(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFrozenPuddleStandsOnTheBedTest,
	"ARPG.World.Fluid.Solidify.AFullyFrozenPuddleStandsOnTheFloor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFrozenPuddleStandsOnTheBedTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;

	// A DEEP puddle, so a slab left at the waterline is unmistakably in the air.
	WaterDefinition->Depth = 40.f;

	UARPGSolidDefinition* IceDefinition = NewObject<UARPGSolidDefinition>();
	IceDefinition->Element = Ice;
	IceDefinition->Thickness = 30.f;
	IceDefinition->bStandable = true;
	IceDefinition->MinimumArea = 100.f;

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { IceDefinition };
	Fluids->CombinationTable = MakeFreezeTable(Ice);

	MakeGround(Scope.World, 0.f);

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 200.f, TAG_Element_Water);
	TestNotNull(TEXT("Setup: a puddle exists"), Pool);
	if (!Pool)
	{
		return false;
	}

	const float Bed = Pool->GetSurfaceBedAt(FVector2D::ZeroVector);
	const float Waterline = Pool->GetSurfaceHeight();

	TestTrue(TEXT("Setup: the waterline is well above the bed"), Waterline - Bed > 30.f);

	// An agent big enough to take the whole puddle, which is the ordinary case:
	// an emanation is metres across and a puddle is not.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Shard = Scope.World->SpawnActor<AActor>(AActor::StaticClass(),
		FTransform(FVector(0, 0, 0)), Params);

	USphereComponent* Sphere = NewObject<USphereComponent>(Shard);
	Sphere->SetSphereRadius(400.f);
	Sphere->SetMobility(EComponentMobility::Movable);
	Sphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Shard->SetRootComponent(Sphere);
	Sphere->RegisterComponent();
	Sphere->SetWorldLocation(FVector(0, 0, 0));

	UARPGElementalVolumeComponent* ShardVolume = NewObject<UARPGElementalVolumeComponent>(Shard);
	ShardVolume->Element = Ice;
	ShardVolume->OverlapSource = Sphere;
	ShardVolume->SetupAttachment(Sphere);
	ShardVolume->RegisterComponent();
	ShardVolume->SetEnergy(50.f);

	TestTrue(TEXT("The whole puddle freezes"), Fluids->TrySolidify(Pool->Volume, ShardVolume));
	TestEqual(TEXT("Leaving no water behind"), Fluids->GetPools().Num(), 0);
	TestEqual(TEXT("And one slab"), Fluids->GetSolids().Num(), 1);

	if (Fluids->GetSolids().Num() == 0)
	{
		return false;
	}

	const AARPGSolidBody* Floe = Fluids->GetSolids()[0];

	// THE POINT. GroundHeight tracks the surface a slab RIDES, and a slab that
	// rides nothing has to take the floor instead -- otherwise it is left at the
	// waterline with no buoyancy tick to settle it, hanging in the air by exactly
	// the depth the puddle had.
	TestEqual(TEXT("The ice stands on the bed, not at the old waterline"),
		static_cast<float>(Floe->GetActorLocation().Z), Bed, 1.f);

	TestTrue(TEXT("So its top is one thickness above the floor"),
		FMath::IsNearlyEqual(Floe->GetSurfaceHeight(), Bed + IceDefinition->Thickness, 1.f));

	// And it stays there rather than being settled or dropped by a later tick.
	Scope.World->Tick(LEVELTICK_All, 0.1f);
	Scope.World->Tick(LEVELTICK_All, 0.1f);

	TestEqual(TEXT("And is still there afterwards"), Fluids->GetSolids().Num(), 1);
	TestEqual(TEXT("Still on the floor"),
		static_cast<float>(Floe->GetActorLocation().Z), Bed, 1.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidOffCentreTest,
	"ARPG.World.Fluid.Solidify.AnOffCentreCastFreezesOnlyWhatItOverlaps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidOffCentreTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	ARPGTest::FTestWorldBegunPlay Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	UARPGElementalReactionSubsystem* Reactions =
		Scope.World->GetSubsystem<UARPGElementalReactionSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;

	UARPGSolidDefinition* IceDefinition = NewObject<UARPGSolidDefinition>();
	IceDefinition->Element = Ice;
	IceDefinition->Thickness = 30.f;
	IceDefinition->bStandable = true;
	IceDefinition->MinimumArea = 100.f;

	UARPGMagicCombinationTable* Table = MakeFreezeTable(Ice);
	UARPGMagicCombinationEntry* Collision = NewObject<UARPGMagicCombinationEntry>(Table);
	Collision->RequiredElements.AddTag(TAG_Element_Ice);
	Collision->RequiredElements.AddTag(TAG_Element_Water);
	Collision->Result = Ice;
	Collision->Mode = EARPGReactionMode::Auto;
	Collision->Scope = static_cast<int32>(EARPGCombinationScope::Collision);
	Table->Entries.Add(Collision);

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { IceDefinition };
	Fluids->CombinationTable = Table;
	Reactions->CombinationTable = Table;

	MakeGround(Scope.World, 0.f);

	// A BIG pool, and the caster standing well off to one side of it.
	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 600.f, TAG_Element_Water);
	const double PoolArea = Pool->GetArea();
	const TArray<FVector2D> PoolRing = Pool->GetRing();

	// A real emanation, spawned the way the discharge ability spawns one, at low
	// power so its 160cm reach is much smaller than the pool it stands in.
	FARPGDischargeContext Context;
	Context.DischargeType = EARPGDischargeType::Emanate;
	Context.PrimaryElement = Ice;
	Context.Origin = FVector(500, 0, 90);
	Context.Direction = FVector::ForwardVector;
	Context.ComputedDamage = 40.f;
	Context.MinPower = 0.f;
	Context.MaxPower = 1.f;
	Context.PowerFraction = 0.f;

	const FTransform Where(Context.Direction.Rotation(), Context.Origin);
	AARPGPlaceholderDischarge* Emanation =
		Scope.World->SpawnActorDeferred<AARPGPlaceholderDischarge>(
			AARPGPlaceholderDischarge::StaticClass(), Where, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	Emanation->InitializeFromContext(Context);
	Emanation->FinishSpawning(Where);

	Scope.World->Tick(LEVELTICK_All, 0.1f);

	// EXACTLY ONE. More than one is the cascade: the slab a freeze produces is
	// itself an ice volume lying on the same water, so it used to be read as a
	// second agent arriving, freeze another piece, and repeat until the pool was
	// gone -- see UARPGElementalReactionSubsystem::Resolve.
	TestEqual(TEXT("One cast freezes one slab"), Fluids->GetSolids().Num(), 1);

	if (Fluids->GetSolids().Num() == 0)
	{
		return false;
	}

	AARPGSolidBody* Floe = Fluids->GetSolids()[0];
	const FVector2D Centre = Floe->GetWorldCentre();

	// The honest answer: the pool clipped against a disc at the caster with the
	// emanation's own reach. Computed here rather than hard-coded so the test
	// still means something if the shape of an emanation changes.
	const double Reach = Emanation->ReactionRadius;
	TArray<FVector2D> Expected;
	TArray<TArray<FVector2D>> Holes;
	ARPGFluidGeometry::IntersectWithHoles(PoolRing,
		ARPGFluidGeometry::MakeCircle(FVector2D(500, 0), Reach), Expected, Holes);
	const double ExpectedArea = ARPGFluidGeometry::PolygonArea(Expected);
	const FVector2D ExpectedCentre = ARPGFluidGeometry::PolygonCentroid(Expected);

	TestTrue(TEXT("Setup: the overlap is a small part of the pool"),
		ExpectedArea < PoolArea * 0.2);

	// WHERE THE SPELL WAS, not where the puddle was. The cascade walked the ice
	// inward cast after cast and left one slab near the pool's middle, which is
	// nowhere near what the player aimed at.
	TestEqual(TEXT("The slab sits at the overlap"),
		static_cast<float>(Centre.X), static_cast<float>(ExpectedCentre.X), 40.f);
	TestEqual(TEXT("In both axes"),
		static_cast<float>(Centre.Y), static_cast<float>(ExpectedCentre.Y), 40.f);

	// A tenth, which is comfortably wider than the quarter-metre grid the field is
	// rasterised onto and far tighter than any cascade.
	TestTrue(FString::Printf(TEXT("And is the size of the overlap (%.0f against %.0f)"),
		Floe->GetArea(), ExpectedArea),
		FMath::Abs(Floe->GetArea() - ExpectedArea) < ExpectedArea * 0.1);

	// AND THE REST OF THE PUDDLE IS STILL THERE. The cascade's signature is a pool
	// that vanishes from a single cast that only touched a corner of it.
	TestEqual(TEXT("The puddle survives"), Fluids->GetPools().Num(), 1);

	if (Fluids->GetPools().Num() > 0)
	{
		TestTrue(TEXT("Having lost only what froze"),
			Fluids->GetPools()[0]->GetArea() > PoolArea * 0.8);
	}

	// Let it settle: the floe rides the pool it froze out of, and a rider that
	// reacts with what it is riding starts the cascade a frame late instead.
	Scope.World->Tick(LEVELTICK_All, 0.1f);
	Scope.World->Tick(LEVELTICK_All, 0.1f);

	TestEqual(TEXT("And nothing else freezes afterwards"), Fluids->GetSolids().Num(), 1);
	TestEqual(TEXT("With the puddle still there"), Fluids->GetPools().Num(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFreezeCutsTheWaterTest,
	"ARPG.World.Fluid.Solidify.TheWaterLosesTheGroundTheIceIsStandingOn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFreezeCutsTheWaterTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->EvaporationRate = 0.f;

	UARPGSolidDefinition* IceDefinition = MakeIce(Ice);
	IceDefinition->MinimumArea = 100.f;

	Fluids->Definitions = { WaterDefinition };
	Fluids->Solids = { IceDefinition };
	Fluids->CombinationTable = MakeFreezeTable(Ice);

	MakeGround(Scope.World, 0.f);

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0, 0, 0), 400.f, TAG_Element_Water);
	TestNotNull(TEXT("Setup: a puddle exists"), Pool);
	if (!Pool)
	{
		return false;
	}

	const double Before = Pool->GetArea();

	// AT THE RIM, so the cut reaches the outline. A patch frozen out of the middle
	// is the other case, and correctly leaves the outline alone -- see
	// AARPGSurfaceBody::ConsumeSurfaceRegion.
	const FVector2D Struck(340, 0);
	const FVector2D Opposite(-340, 0);

	TestTrue(TEXT("Setup: there is water where the ice will form"), Pool->IsSurfaceAt(Struck));
	TestTrue(TEXT("Setup: and water on the far side"), Pool->IsSurfaceAt(Opposite));

	UARPGElementalVolumeComponent* Shard =
		MakeShard(Scope.World, Ice, FVector(Struck.X, Struck.Y, 0), 150.f);

	TestTrue(TEXT("It freezes"), Fluids->TrySolidify(Pool->Volume, Shard));
	TestEqual(TEXT("Into one slab"), Fluids->GetSolids().Num(), 1);

	if (Fluids->GetPools().Num() == 0)
	{
		AddError(TEXT("The puddle should have survived a bite out of its rim."));
		return false;
	}

	// THE POINT. The ice occupies that patch, so the water is no longer there --
	// it did not simply shrink somewhere else and leave the floe sitting on top of
	// water that had never receded.
	TestFalse(TEXT("The water is gone from under the ice"), Pool->IsSurfaceAt(Struck));

	// AND ONLY THERE. A uniform shrink takes the same ring off every side at once,
	// which would have emptied the far rim as well; a cut takes it from where the
	// ice actually is.
	TestTrue(TEXT("But still there on the far side"), Pool->IsSurfaceAt(Opposite));

	TestTrue(TEXT("And the puddle genuinely lost ground"), Pool->GetArea() < Before);

	// The slab is standing exactly where the water stopped being.
	TestTrue(TEXT("The ice is where the water was"),
		Fluids->GetSolids()[0]->IsStandableAt(
			FVector(Struck.X, Struck.Y, Fluids->GetSolids()[0]->GetSurfaceHeight())));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSlabBreaksWhereHitTest,
	"ARPG.World.Fluid.Slabs.ASpellCutsABowlWhereItHitAndBreaksThrough",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSlabBreaksWhereHitTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	UARPGMagicElement* Earth = MakeElement(GetTransientPackage(), TAG_Element_Earth);
	UARPGSolidDefinition* EarthDefinition = MakeEarth(Earth);
	Fluids->Solids = { EarthDefinition };

	AARPGSolidBody* Wall = RaiseSlab(Scope.World, Fluids, EarthDefinition,
		FVector2D::ZeroVector, 200.0);

	const double Before = Wall->GetArea();
	const float Standing = Wall->GetSurfaceHeight();

	const FVector Struck(150, 0, Standing);
	const FVector Opposite(-150, 0, Standing);

	TestTrue(TEXT("Setup: you can stand where it will be hit"), Wall->IsStandableAt(Struck));
	TestTrue(TEXT("Setup: and on the far side"), Wall->IsStandableAt(Opposite));

	// WHAT THE REACTION SOLVER DOES ON ITS WAY PAST: it knows the contact point,
	// and the hook it is about to call has no room in its signature to carry one.
	Wall->NoteContactAt(FVector2D(Struck.X, Struck.Y));

	// Energy enough for a bite of about 80cm. Ground = Consumed / EnergyPerArea,
	// so this is the reaction paying for roughly 20,000 square cm of wall.
	Wall->OnElementalReaction_Implementation(
		/*Consumed=*/402.f, /*Remaining=*/0.5f, /*Product=*/nullptr);

	TestEqual(TEXT("The wall is still standing"), Fluids->GetSolids().Num(), 1);

	// THE POINT. It lost the piece that was struck.
	TestFalse(TEXT("The struck side is gone"), Wall->IsStandableAt(Struck));

	// AND NOT THE REST OF IT. Shrinking uniformly takes the same ring off every
	// side, so the far side would have gone too -- which is what made a fireball
	// read as making the whole wall smaller rather than breaking a piece off it.
	TestTrue(TEXT("The far side is untouched"), Wall->IsStandableAt(Opposite));

	TestTrue(TEXT("And it genuinely lost ground"), Wall->GetArea() < Before);

	// A HIT IN THE MIDDLE IS A BOWL BEFORE IT IS A HOLE. This is the thing a slab
	// lost when it stopped being a heightfield and got back when it stopped being
	// drawn as a prism: the top is a height function sampled per vertex, so it can
	// hold a dish, and only a dish deep enough to reach the underside takes any
	// ground at all.
	const double BeforeDish = Wall->GetArea();
	const float Rim = Wall->GetSurfaceHeight();

	Wall->NoteContactAt(FVector2D::ZeroVector);
	Wall->OnElementalReaction_Implementation(150.f, 0.5f, nullptr);

	TestTrue(TEXT("A hit in the middle cuts a bowl"), Wall->GetBites().Num() > 0);
	TestTrue(TEXT("The middle is lower than it was"),
		Wall->GetSurfaceLevelAt(FVector2D::ZeroVector) < Rim - 1.f);

	// A BOWL, not a stamped-out cylinder: deepest at the middle and tapering out,
	// which is what makes a hit at the edge of a slab cut it away at an angle.
	TestTrue(TEXT("And deeper at its centre than at its rim"),
		Wall->BiteDepthAt(FVector2D::ZeroVector) > Wall->BiteDepthAt(FVector2D(70, 0)));
	// Clear of BOTH craters: the first landed at (150, 0) and reaches 90cm from
	// there, so anywhere near the struck rim is legitimately dished already.
	TestTrue(TEXT("With the slab untouched away from either"),
		FMath::IsNearlyZero(Wall->BiteDepthAt(FVector2D(-150, 0)), 0.01f));

	// AND YOU CAN STILL STAND IN IT, lower down. A dish is not a gap; there is
	// still slab underneath.
	TestTrue(TEXT("You can stand in the bowl"),
		Wall->IsStandableAt(FVector(0, 0, Standing)));
	TestTrue(TEXT("Which cost the slab no ground at all"),
		FMath::IsNearlyEqual(Wall->GetArea(), BeforeDish, 1.0));

	TestFalse(TEXT("And has not opened a gap"), Wall->GetHole().Num() >= 3);

	// KEEP HITTING THE SAME SPOT AND IT GOES THROUGH. Repeated fire at one place
	// deepens the bowl it already made rather than recording a new one beside it,
	// which is what makes concentrated fire break a wall where scattered fire only
	// scars it.
	for (int32 Shot = 0; Shot < 6; ++Shot)
	{
		Wall->NoteContactAt(FVector2D::ZeroVector);
		Wall->OnElementalReaction_Implementation(150.f, 0.5f, nullptr);
	}

	TestTrue(TEXT("Sustained fire breaks through"), Wall->GetHole().Num() >= 3);
	TestFalse(TEXT("And you cannot stand in the gap"),
		Wall->IsStandableAt(FVector(0, 0, Standing)));
	TestTrue(TEXT("Which now counts against the ground it covers"),
		Wall->GetArea() < BeforeDish);

	// Still one crater rather than seven, so the mesh stays bounded however long
	// the fight goes on.
	TestTrue(TEXT("Recorded as one crater, not seven"), Wall->GetBites().Num() <= 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFreezeDoesNotCascadeTest,
	"ARPG.World.Fluid.Reservoir.FreezingARiverDoesNotRunAway",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFreezeDoesNotCascadeTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	UARPGElementalReactionSubsystem* Reactions =
		Scope.World->GetSubsystem<UARPGElementalReactionSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGMagicElement* Ice = MakeElement(GetTransientPackage(), TAG_Element_Ice);

	UARPGSolidDefinition* IceDefinition = MakeIce(Ice);
	IceDefinition->MinimumArea = 100.f;

	UARPGMagicCombinationTable* Table = MakeFreezeTable(Ice);

	// The Collision row the solver reads on its way to the solidify hand-off.
	UARPGMagicCombinationEntry* Collision = NewObject<UARPGMagicCombinationEntry>(Table);
	Collision->RequiredElements.AddTag(TAG_Element_Ice);
	Collision->RequiredElements.AddTag(TAG_Element_Water);
	Collision->Result = Ice;
	Collision->Mode = EARPGReactionMode::Auto;
	Collision->Scope = static_cast<int32>(EARPGCombinationScope::Collision);
	Table->Entries.Add(Collision);

	Fluids->Solids = { IceDefinition };
	Fluids->CombinationTable = Table;
	Reactions->CombinationTable = Table;

	// A RIVER, which is the case that made this a hang rather than a wrong answer.
	// A reservoir is bottomless: freezing takes nothing off it, so nothing about
	// running out of water was ever going to stop a runaway.
	UARPGReservoirVolumeComponent* River =
		MakeRiver(Scope.World, Water, FVector(0, 0, 0), FVector(2000, 400, 350));

	UARPGElementalVolumeComponent* Shard = MakeShard(Scope.World, Ice, FVector(0, 0, 0), 150.f);

	TestTrue(TEXT("Ice meeting a river freezes it"), Fluids->TrySolidify(River, Shard));
	TestEqual(TEXT("Producing one floe"), Fluids->GetSolids().Num(), 1);

	AARPGSolidBody* Floe = Fluids->GetSolids()[0];

	// THE FLOE IS NOW AN ICE VOLUME LYING ON WATER, which is exactly what the
	// solver used to read as a fresh agent arriving. It froze another patch; that
	// patch was another floe with a full tank of its own; and on a surface that
	// cannot be used up it never stopped. The game halted inside one frame and
	// spawned slabs until it ran out of memory.
	//
	// THE GUARD IS A COMPARISON THAT HAD TO COVER BOTH SHAPES. A surface is
	// sometimes the volume component and sometimes the owning actor -- see
	// FindSurface -- and FloatsOn holds whichever it was given. Testing it against
	// the owner alone was right for a puddle and never true for a river.
	// THE DEFECT ITSELF, asserted where it lived. FindSurface hands back the
	// VOLUME when the volume implements the interface, which is what a reservoir
	// does, and the OWNING ACTOR otherwise, which is what a puddle is. FloatsOn
	// holds whichever it was given -- so a guard that compared it against the
	// owner alone was right for a puddle and could never be true for a river.
	TestTrue(TEXT("A floe on a river rides the volume, not the actor"),
		Floe->FloatsOn.GetObject() == River);
	TestFalse(TEXT("Which is NOT what the volume's owner is"),
		Floe->FloatsOn.GetObject() == River->GetOwner());

	Reactions->Resolve(River, Floe->Volume);

	TestEqual(TEXT("The floe does not freeze the river it is riding"),
		Fluids->GetSolids().Num(), 1);

	// And the other way round, since the solver is handed the pair in either order.
	Reactions->Resolve(Floe->Volume, River);

	TestEqual(TEXT("In either order"), Fluids->GetSolids().Num(), 1);

	// AND THE BACKSTOP UNDER IT. Even handed a fresh agent with its own energy,
	// nothing may freeze ground that is already ice -- so no future variant of
	// this can spin, whatever it is that reaches TrySolidify.
	UARPGElementalVolumeComponent* Again = MakeShard(Scope.World, Ice, FVector(0, 0, 0), 150.f);

	TestFalse(TEXT("Nothing freezes what is already frozen"),
		Fluids->TrySolidify(River, Again));
	TestEqual(TEXT("So the river still carries one floe"), Fluids->GetSolids().Num(), 1);

	// Clear of the first, though, is still a river anyone can freeze.
	UARPGElementalVolumeComponent* Elsewhere =
		MakeShard(Scope.World, Ice, FVector(700, 0, 0), 150.f);

	TestTrue(TEXT("But open water still freezes"), Fluids->TrySolidify(River, Elsewhere));
	TestEqual(TEXT("Making a second floe"), Fluids->GetSolids().Num(), 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFireOnEarthTest,
	"ARPG.World.Fluid.Slabs.FireCarvesABowlAndLeavesTheRockItMelted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFireOnEarthTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	UARPGElementalReactionSubsystem* Reactions =
		Scope.World->GetSubsystem<UARPGElementalReactionSubsystem>();

	UARPGMagicElement* Earth = MakeElement(GetTransientPackage(), TAG_Element_Earth);
	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire);
	UARPGMagicElement* Lava = MakeElement(GetTransientPackage(), TAG_Element_Lava);

	UARPGFluidDefinition* LavaDefinition = MakeLava(GetTransientPackage(), Lava);

	UARPGSolidDefinition* EarthDefinition = MakeEarth(Earth);
	EarthDefinition->BreaksInto = LavaDefinition;
	EarthDefinition->BreakRadius = 70.f;

	// Fire + earth -> lava, which is a COLLISION row: the reaction spawns a lava
	// product of its own, and that is the other thing that used to deposit.
	UARPGMagicCombinationTable* Table = NewObject<UARPGMagicCombinationTable>();
	UARPGMagicCombinationEntry* ToLava = NewObject<UARPGMagicCombinationEntry>(Table);
	ToLava->RequiredElements.AddTag(TAG_Element_Fire);
	ToLava->RequiredElements.AddTag(TAG_Element_Earth);
	ToLava->Result = Lava;
	ToLava->Mode = EARPGReactionMode::Auto;
	ToLava->Scope = static_cast<int32>(EARPGCombinationScope::Collision);
	Table->Entries.Add(ToLava);

	Fluids->Definitions = { LavaDefinition };
	Fluids->Solids = { EarthDefinition };
	Fluids->CombinationTable = Table;
	Reactions->CombinationTable = Table;

	MakeGround(Scope.World, 0.f);

	AARPGSolidBody* Slab = RaiseSlab(Scope.World, Fluids, EarthDefinition,
		FVector2D::ZeroVector, 200.0);

	const double SlabArea = Slab->GetArea();
	const int32 FlatTriangles = Slab->GetSurfaceTriangleCount();

	// AT THE CORNER, which is where this was reported: a hit mostly off the edge
	// of the slab, where the region asked for is far bigger than the rock actually
	// there.
	const FVector2D Struck(180, 0);
	const FVector2D Opposite(-180, 0);

	const float RimBefore = Slab->GetSurfaceLevelAt(Struck);
	const float FarBefore = Slab->GetSurfaceLevelAt(Opposite);

	UARPGElementalVolumeComponent* Ball =
		MakeShard(Scope.World, Fire, FVector(Struck.X, Struck.Y, Slab->GetSurfaceHeight()), 90.f);

	// Enough to dish it without going through, which is the ordinary case.
	Ball->SetEnergy(60.f);

	Reactions->Resolve(Slab->Volume, Ball);

	// --- THE BOWL --------------------------------------------------------------

	TestEqual(TEXT("One crater, where it was hit"), Slab->GetBites().Num(), 1);

	TestTrue(TEXT("The surface is lower where it was struck"),
		Slab->GetSurfaceLevelAt(Struck) < RimBefore - 1.f);
	TestEqual(TEXT("And unchanged on the far side"),
		Slab->GetSurfaceLevelAt(Opposite), FarBefore, 0.5f);

	// A dish rather than a stamped-out cylinder.
	TestTrue(TEXT("Deepest at the middle of the crater"),
		Slab->BiteDepthAt(Struck) > Slab->BiteDepthAt(Struck + FVector2D(50, 0)));

	// AND THE MESH FOLLOWED IT. A bowl is a height function sampled per vertex, so
	// it exists only as far as the cap was tessellated to carry it -- a slab still
	// drawn as a flat lid would report a dish that nobody could see.
	TestTrue(FString::Printf(TEXT("The drawn surface gained vertices to hold it (%d -> %d)"),
		FlatTriangles, Slab->GetSurfaceTriangleCount()),
		Slab->GetSurfaceTriangleCount() > FlatTriangles);

	// --- AND THE LAVA ----------------------------------------------------------

	TestEqual(TEXT("It leaves exactly one body of lava"), Fluids->GetPools().Num(), 1);

	if (Fluids->GetPools().Num() != 1)
	{
		return false;
	}

	// EQUAL TO WHAT THE BOWL TOOK, by mass. Rock is denser than the lava it melts
	// into, so a cubic metre of wall is the volume of lava that weighs the same.
	const FARPGSlabBite& Crater = Slab->GetBites()[0];
	const double Removed = PI * Crater.Radius * Crater.Radius * Crater.Depth * 0.5;
	const double Expected = Removed * EarthDefinition->Density / LavaDefinition->Density;

	const double Poured = Fluids->GetPools()[0]->GetArea() * LavaDefinition->Depth;

	TestTrue(FString::Printf(
		TEXT("The lava is the rock that melted, not the size of the spell (%.0f against %.0f)"),
		Poured, Expected), FMath::Abs(Poured - Expected) < Expected * 0.15);

	TestTrue(TEXT("Which is nowhere near the size of the slab"),
		Fluids->GetPools()[0]->GetArea() < SlabArea);

	// AND THE LEDGER SAYS SO, which is the half of this the pool count cannot
	// show. The reaction's own product deposits the SAME fluid when its discharge
	// lands, sized by how big the spell was rather than by the rock that melted --
	// fire + earth is one body of lava, not two. Saying it has been accounted for
	// is what stops the product handing back a second, far larger one.
	//
	// Asserted directly because the product needs a discharge effect class to
	// spawn at all, which a fixture has no reason to wire up: the pool count above
	// would stay at one here whether this was said or not.
	TestTrue(TEXT("The lava is recorded as already accounted for"),
		Fluids->WasFluidReturned(Lava->ElementTag));

	// A dish that did not reach the underside takes no ground at all.
	TestEqual(TEXT("And a dish costs the slab no footing"), Slab->GetArea(), SlabArea, 1.0);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGSlabFacesOutwardTest,
	"ARPG.World.Fluid.Slabs.ARaisedSlabIsSolidFromTheOutside",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGSlabFacesOutwardTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	UARPGMagicElement* Earth = MakeElement(GetTransientPackage(), TAG_Element_Earth);
	UARPGSolidDefinition* EarthDefinition = MakeEarth(Earth);
	Fluids->Solids = { EarthDefinition };

	AARPGSolidBody* Slab = RaiseSlab(Scope.World, Fluids, EarthDefinition,
		FVector2D::ZeroVector, 200.0);

	// WHICH WAY THE FACES POINT, ASKED THE WAY THE GAME ASKS IT. A slab's drawn
	// surface IS its collision, so a trace is the same question as "can you see
	// this" and "can you stand on this" at once -- and both had the same wrong
	// answer while the mesh was wound inside out. Unreal is left-handed and
	// FDynamicMesh3 takes a normal as (C - A) x (B - A), so the winding that
	// reads as upward-facing in every other library points a face at the floor.
	const float Top = EarthDefinition->Thickness;

	// The middle, off-centre, and out near the rim: a cap wound backwards is
	// backwards everywhere, but so is a single cell, and only sampling the
	// centre would miss a rim built from a different branch.
	const FVector2D Spots[] = { FVector2D(0, 0), FVector2D(60, 0), FVector2D(-100, -100) };

	for (const FVector2D& Spot : Spots)
	{
		const FVector Above(Spot.X, Spot.Y, Top + 400.f);
		const FVector Below(Spot.X, Spot.Y, -400.f);

		FHitResult Hit;
		const bool bHit = Scope.World->LineTraceSingleByChannel(Hit, Above, Below,
			ECC_Visibility, FCollisionQueryParams(SCENE_QUERY_STAT(SlabFaces), false));

		// LANDING ON IT, not falling into it. Wound the other way the first thing
		// a downward trace met was the UNDERSIDE at Z=0 -- which is exactly what
		// jumping onto the slab and ending up inside it looks like.
		TestTrue(TEXT("A trace from above is stopped"), bHit);
		TestEqual(TEXT("At the top of the slab"),
			static_cast<float>(Hit.ImpactPoint.Z), Top, 0.5f);
		TestTrue(TEXT("By a surface facing up at it"), Hit.ImpactNormal.Z > 0.9);
	}

	// The underside is the same test from the other side: a closed body is solid
	// from every direction, and a mesh flipped wholesale still passes a test that
	// only ever looks down.
	FHitResult FromBelow;
	TestTrue(TEXT("A trace from underneath is stopped"),
		Scope.World->LineTraceSingleByChannel(FromBelow, FVector(0, 0, -400), FVector(0, 0, Top + 400),
			ECC_Visibility, FCollisionQueryParams(SCENE_QUERY_STAT(SlabFaces), false)));
	TestEqual(TEXT("At the underside"),
		static_cast<float>(FromBelow.ImpactPoint.Z), 0.f, 0.5f);
	TestTrue(TEXT("By a surface facing down at it"), FromBelow.ImpactNormal.Z < -0.9);

	// And the walls, which is where being inside out is invisible from directly
	// above: a trace across the slab used to pass through the near wall and stop
	// on the far one from the inside.
	FHitResult Across;
	TestTrue(TEXT("A trace across it is stopped"),
		Scope.World->LineTraceSingleByChannel(Across, FVector(-800, 0, Top * 0.5f),
			FVector(800, 0, Top * 0.5f), ECC_Visibility,
			FCollisionQueryParams(SCENE_QUERY_STAT(SlabFaces), false)));
	TestTrue(TEXT("On the near wall rather than the far one"), Across.ImpactPoint.X < 0.0);
	TestTrue(TEXT("By a surface facing back the way the trace came"),
		Across.ImpactNormal.X < -0.9);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPoolStopsAtALedgeTest,
	"ARPG.World.Fluid.Deposit.APoolStopsAtTheEdgeOfWhatHoldsIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPoolStopsAtALedgeTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->MinimumArea = 100.f;
	Fluids->Definitions = { WaterDefinition };

	// A PLATFORM WITH ONE EDGE, which is the stage this came from reduced to the
	// part that matters: floor out to X = 0 and a drop past it. An emanation cast
	// near that lip covers ground on one side and thin air on the other.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* Platform = Scope.World->SpawnActor<AActor>(
		AActor::StaticClass(), FTransform::Identity, Params);

	UBoxComponent* Floor = NewObject<UBoxComponent>(Platform);
	Floor->SetupAttachment(Platform->GetRootComponent());
	Floor->RegisterComponent();
	Floor->SetBoxExtent(FVector(500.f, 500.f, 50.f));

	// Top face at Z = 0, and its far edge at X = 0.
	Floor->SetWorldLocation(FVector(-500.f, 0.f, -50.f));
	Floor->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Floor->SetCollisionObjectType(ECC_WorldStatic);
	Floor->SetCollisionResponseToAllChannels(ECR_Block);

	// Cast ON the platform, one radius back from the lip, so half the disc would
	// reach out over the drop.
	const float Radius = 200.f;
	AARPGFluidPool* Pool = Fluids->Deposit(FVector(-100.f, 0.f, 0.f), Radius, TAG_Element_Water);

	if (!Pool)
	{
		AddError(TEXT("Nothing pooled on the platform at all"));
		return false;
	}

	// NOT OUT OVER THE DROP. This is the whole report: the far half of the disc
	// used to be a flat surface with nothing underneath it, because the deposit
	// sampled the ground ONCE, at the middle, and laid the entire footprint at
	// that one height.
	const FBox2D Bounds = ARPGFluidGeometry::PolygonBounds(Pool->GetRing());

	TestTrue(FString::Printf(TEXT("It stops at the lip rather than hanging past it (reached %.0f)"),
		Bounds.Max.X), Bounds.Max.X <= 30.f);

	// AND IS STILL A PUDDLE. Trimming that ate the whole body would "fix" this by
	// making water stop working near anything interesting.
	TestTrue(TEXT("And is still a body worth having"),
		ARPGFluidGeometry::PolygonArea(Pool->GetRing()) > 10000.0);

	// The half that IS on the platform keeps its reach, so the trim took the
	// unsupported side and not the outline generally.
	TestTrue(FString::Printf(TEXT("Keeping its reach back onto the floor (%.0f)"), Bounds.Min.X),
		Bounds.Min.X < -250.f);

	// EVERY POINT OF IT ON SOMETHING. Bounds are a summary; this is the claim.
	for (const FVector2D& Point : Pool->GetRing())
	{
		FHitResult Hit;
		const bool bGround = Scope.World->LineTraceSingleByChannel(Hit,
			FVector(Point.X, Point.Y, 50.f), FVector(Point.X, Point.Y, -400.f),
			ECC_Visibility, FCollisionQueryParams(SCENE_QUERY_STAT(PoolLedge), false));

		if (!bGround)
		{
			AddError(FString::Printf(TEXT("The pool reaches (%.0f, %.0f), where there is no floor"),
				Point.X, Point.Y));
			return false;
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPoolOnFlatGroundTest,
	"ARPG.World.Fluid.Deposit.FlatGroundTakesTheWholeFootprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPoolOnFlatGroundTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->MinimumArea = 100.f;
	Fluids->Definitions = { WaterDefinition };

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* Ground = Scope.World->SpawnActor<AActor>(
		AActor::StaticClass(), FTransform::Identity, Params);

	UBoxComponent* Floor = NewObject<UBoxComponent>(Ground);
	Floor->SetupAttachment(Ground->GetRootComponent());
	Floor->RegisterComponent();
	Floor->SetBoxExtent(FVector(2000.f, 2000.f, 50.f));
	Floor->SetWorldLocation(FVector(0.f, 0.f, -50.f));
	Floor->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Floor->SetCollisionObjectType(ECC_WorldStatic);
	Floor->SetCollisionResponseToAllChannels(ECR_Block);

	const float Radius = 200.f;
	AARPGFluidPool* Pool = Fluids->Deposit(FVector::ZeroVector, Radius, TAG_Element_Water);

	if (!Pool)
	{
		AddError(TEXT("Nothing pooled on open ground"));
		return false;
	}

	// THE OTHER HALF OF THE LEDGE CASE, and the one that would go unnoticed: a
	// trim that quietly shaved every puddle in the game would still pass the test
	// above. On a floor with nothing to stop it, the footprint is untouched.
	const double Asked = ARPGFluidGeometry::PolygonArea(
		ARPGFluidGeometry::MakeCircle(FVector2D::ZeroVector, Radius));

	TestEqual(TEXT("An unobstructed puddle is exactly what was asked for"),
		ARPGFluidGeometry::PolygonArea(Pool->GetRing()), Asked, Asked * 0.01);

	return true;
}

namespace ARPGFluidTestUtils
{
	/**
	 * A ramp, built out of steps small enough that the trim treats it as one
	 * continuous floor.
	 *
	 * A rotated box would be a truer ramp, but a stack of thin treads is exactly
	 * as good for the question being asked -- does the body follow the floor down
	 * -- and it lets a test say where the floor is at a given X without doing
	 * trigonometry to work out what it just built.
	 */
	inline AActor* BuildRamp(UWorld* World, float FromX, float ToX, float Fall, int32 Treads)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AActor* Ramp = World->SpawnActor<AActor>(
			AActor::StaticClass(), FTransform::Identity, Params);

		const float Width = (ToX - FromX) / Treads;

		for (int32 Tread = 0; Tread < Treads; ++Tread)
		{
			const float MidX = FromX + Width * (Tread + 0.5f);
			const float TopZ = -Fall * (Tread + 0.5f) / Treads;

			UBoxComponent* Step = NewObject<UBoxComponent>(Ramp);
			Step->SetupAttachment(Ramp->GetRootComponent());
			Step->RegisterComponent();
			Step->SetBoxExtent(FVector(Width * 0.5f, 600.f, 400.f));
			Step->SetWorldLocation(FVector(MidX, 0.f, TopZ - 400.f));
			Step->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Step->SetCollisionObjectType(ECC_WorldStatic);
			Step->SetCollisionResponseToAllChannels(ECR_Block);
		}

		return Ramp;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPoolFollowsASlopeTest,
	"ARPG.World.Fluid.Deposit.APoolFollowsTheSlopeItLiesOn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPoolFollowsASlopeTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->MinimumArea = 100.f;
	WaterDefinition->Depth = 20.f;
	Fluids->Definitions = { WaterDefinition };

	// A METRE OF FALL over four metres of run, which is the kind of ramp a stage
	// has between an upper platform and the floor below it.
	const float Fall = 100.f;
	BuildRamp(Scope.World, -400.f, 400.f, Fall, 40);

	// Cast in the middle, so the disc reaches well up the ramp and well down it.
	const float Radius = 250.f;
	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0.f, 0.f, -Fall * 0.5f), Radius,
		TAG_Element_Water);

	if (!Pool)
	{
		AddError(TEXT("Nothing pooled on the ramp"));
		return false;
	}

	// IT DID NOT GET TRIMMED AWAY. The first thing a slope-blind body does on a
	// ramp is refuse to exist: every point of it is at a different height from
	// the middle, so a trim measured against the middle eats the whole outline.
	const FBox2D Bounds = ARPGFluidGeometry::PolygonBounds(Pool->GetRing());

	TestTrue(FString::Printf(TEXT("It kept its reach up the ramp (%.0f)"), Bounds.Min.X),
		Bounds.Min.X < -200.f);
	TestTrue(FString::Printf(TEXT("And down it (%.0f)"), Bounds.Max.X),
		Bounds.Max.X > 200.f);

	// AND IT LIES ON THE RAMP. The floor drops 12.5cm per metre, so two points two
	// metres apart along the fall line differ by about 25cm -- and the pool's
	// surface has to differ by the same, or it is a flat lid over a slope.
	const float Uphill = Pool->GetSurfaceLevelAt(FVector2D(-200.f, 0.f));
	const float Downhill = Pool->GetSurfaceLevelAt(FVector2D(200.f, 0.f));

	TestTrue(FString::Printf(TEXT("The uphill end is higher than the downhill end (%.1f vs %.1f)"),
		Uphill, Downhill), Uphill > Downhill + 30.f);

	// EVERYWHERE, not just at the ends: the surface tracks the floor at a constant
	// depth rather than tilting by some amount of its own.
	for (float X = -200.f; X <= 200.f; X += 50.f)
	{
		const FVector2D At(X, 0.f);

		float Floor = 0.f;
		if (!Fluids->FindGroundAt(At, 200.f, Floor, Pool))
		{
			AddError(FString::Printf(TEXT("No ramp under (%.0f, 0)"), X));
			return false;
		}

		TestEqual(FString::Printf(TEXT("The bed follows the floor at (%.0f, 0)"), X),
			Pool->GetSurfaceBedAt(At), Floor, 8.f);

		TestEqual(FString::Printf(TEXT("And the surface rides its own depth above it at (%.0f, 0)"), X),
			Pool->GetSurfaceLevelAt(At) - Pool->GetSurfaceBedAt(At), WaterDefinition->Depth, 0.1f);
	}

	// THE DRAWN MESH FOLLOWS IT TOO, which is the part the player sees and the
	// only part none of the above can speak for -- the height queries answer from
	// the bed directly, so they read correctly even while the mesh is a flat lid.
	//
	// Asked as the mesh's own Z extent: a flat pool is exactly its own depth
	// thick, and one laid down this ramp spans the fall as well.
	TestTrue(TEXT("And there is a mesh with enough detail to bend"),
		Pool->GetSurfaceTriangleCount() > 40);

	UPrimitiveComponent* Drawn = Pool->GetSurfaceComponent();
	const float Thickness = Drawn
		? static_cast<float>(Drawn->CalcBounds(Drawn->GetComponentTransform()).BoxExtent.Z * 2.0)
		: 0.f;

	TestTrue(FString::Printf(TEXT("And the drawn surface spans the fall, not just its depth (%.1f)"),
		Thickness), Thickness > WaterDefinition->Depth + 40.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPoolStillStopsAtACliffTest,
	"ARPG.World.Fluid.Deposit.FollowingASlopeDoesNotMeanCrossingACliff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPoolStillStopsAtACliffTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->MinimumArea = 100.f;
	Fluids->Definitions = { WaterDefinition };

	// THE SAME FALL AS THE RAMP ABOVE, taken all at once. This is the pair that
	// makes the rule meaningful: following the floor has to mean following it
	// down a slope WITHOUT meaning bridging a drop of the same size.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* Stage = Scope.World->SpawnActor<AActor>(
		AActor::StaticClass(), FTransform::Identity, Params);

	UBoxComponent* Upper = NewObject<UBoxComponent>(Stage);
	Upper->SetupAttachment(Stage->GetRootComponent());
	Upper->RegisterComponent();
	Upper->SetBoxExtent(FVector(400.f, 600.f, 50.f));
	Upper->SetWorldLocation(FVector(-400.f, 0.f, -50.f));
	Upper->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Upper->SetCollisionObjectType(ECC_WorldStatic);
	Upper->SetCollisionResponseToAllChannels(ECR_Block);

	UBoxComponent* Lower = NewObject<UBoxComponent>(Stage);
	Lower->SetupAttachment(Stage->GetRootComponent());
	Lower->RegisterComponent();
	Lower->SetBoxExtent(FVector(400.f, 600.f, 50.f));
	Lower->SetWorldLocation(FVector(400.f, 0.f, -150.f));
	Lower->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Lower->SetCollisionObjectType(ECC_WorldStatic);
	Lower->SetCollisionResponseToAllChannels(ECR_Block);

	const float Radius = 250.f;
	AARPGFluidPool* Pool = Fluids->Deposit(FVector(-100.f, 0.f, 0.f), Radius, TAG_Element_Water);

	if (!Pool)
	{
		AddError(TEXT("Nothing pooled on the upper step"));
		return false;
	}

	const FBox2D Bounds = ARPGFluidGeometry::PolygonBounds(Pool->GetRing());

	// A metre of drop in one step is a ledge, and the water stops on top of it.
	TestTrue(FString::Printf(TEXT("It stops at the lip (%.0f)"), Bounds.Max.X),
		Bounds.Max.X <= 60.f);

	TestTrue(TEXT("And is still a body worth having"),
		ARPGFluidGeometry::PolygonArea(Pool->GetRing()) > 10000.0);

	return true;
}

namespace ARPGFluidTestUtils
{
	/**
	 * A slab of floor, top face at TopZ, centred on (X, 0).
	 *
	 * THIN BY DEFAULT, which matters more than it looks: a balcony modelled as a
	 * block thick enough to reach the floor below is not a balcony, it is a
	 * plinth -- and a ground probe under it starts inside solid geometry and
	 * finds nothing, which is a fault in the stage and not in the fluid.
	 */
	inline void AddFloor(AActor* Owner, float X, float TopZ, float HalfX,
		float HalfY = 600.f, float HalfZ = 50.f)
	{
		UBoxComponent* Box = NewObject<UBoxComponent>(Owner);
		Box->SetupAttachment(Owner->GetRootComponent());
		Box->RegisterComponent();
		Box->SetBoxExtent(FVector(HalfX, HalfY, HalfZ));
		Box->SetWorldLocation(FVector(X, 0.f, TopZ - HalfZ));
		Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPoolsOnTwoFloorsTest,
	"ARPG.World.Fluid.Deposit.APuddleUnderABalconyIsNotThePuddleOnIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPoolsOnTwoFloorsTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->MinimumArea = 100.f;
	WaterDefinition->MergeDistance = 200.f;
	Fluids->Definitions = { WaterDefinition };

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Stage = Scope.World->SpawnActor<AActor>(
		AActor::StaticClass(), FTransform::Identity, Params);

	// TWO FLOORS, ONE ABOVE THE OTHER, overlapping in plan -- the elevated middle
	// section of a stage and the lower area it looks down on. The upper one is a
	// balcony over the left half; the ground floor runs the whole width.
	AddFloor(Stage, 0.f, -400.f, 800.f);   // the lower level
	AddFloor(Stage, -400.f, 0.f, 400.f);   // the platform above its left half

	// A puddle on the platform first, so there is something for the next cast to
	// be wrongly swallowed by.
	AARPGFluidPool* Upstairs = Fluids->Deposit(FVector(-300.f, 0.f, 0.f), 150.f, TAG_Element_Water);

	if (!Upstairs)
	{
		AddError(TEXT("Nothing pooled on the platform"));
		return false;
	}

	TestEqual(TEXT("The platform puddle is on the platform"),
		static_cast<float>(Upstairs->GetActorLocation().Z), 0.f, 20.f);

	// NOW CAST DOWNSTAIRS, under the balcony. In plan this lands right beside the
	// puddle above -- inside its bounds plus the merge distance -- which used to
	// be the entire test for "these are the same puddle".
	const int32 Before = Fluids->GetPools().Num();
	AARPGFluidPool* Downstairs = Fluids->Deposit(FVector(-300.f, 0.f, -400.f), 150.f,
		TAG_Element_Water);

	if (!Downstairs)
	{
		AddError(TEXT("Nothing pooled on the lower level"));
		return false;
	}

	// A SECOND BODY, not a bigger first one.
	TestEqual(TEXT("The lower level gets a puddle of its own"),
		Fluids->GetPools().Num(), Before + 1);
	TestTrue(TEXT("Which is not the one upstairs"), Downstairs != Upstairs);

	// AND IT IS WHERE THE SPELL WAS. This is the report: the water appeared at the
	// elevated section instead of at the cast, because the merge grew the balcony's
	// outline to swallow a footprint four metres below it.
	TestEqual(TEXT("At the height it was cast at"),
		static_cast<float>(Downstairs->GetActorLocation().Z), -400.f, 20.f);

	TestEqual(TEXT("And under the point it was cast at"),
		static_cast<float>(Downstairs->GetActorLocation().X), -300.f, 60.f);

	// The one upstairs is untouched -- it did not grow a limb down the stairwell.
	TestTrue(TEXT("And the platform puddle kept its own size"),
		ARPGFluidGeometry::PolygonBounds(Upstairs->GetRing()).GetExtent().X < 200.f);

	// Two puddles on the SAME floor still merge, which is the behaviour the height
	// test must not have broken.
	const int32 Now = Fluids->GetPools().Num();
	Fluids->Deposit(FVector(-200.f, 0.f, -400.f), 150.f, TAG_Element_Water);

	TestEqual(TEXT("But a second puddle beside it on the same floor still merges"),
		Fluids->GetPools().Num(), Now);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPoolUndersideBuriedTest,
	"ARPG.World.Fluid.Deposit.TheUndersideOfAPuddleStaysUnderTheFloor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPoolUndersideBuriedTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->MinimumArea = 100.f;
	WaterDefinition->Depth = 20.f;
	Fluids->Definitions = { WaterDefinition };

	// A ramp, so the bed is sampled and interpolated rather than being one plane
	// the mesh can sit exactly on.
	BuildRamp(Scope.World, -400.f, 400.f, 100.f, 40);

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(0.f, 0.f, -50.f), 250.f, TAG_Element_Water);

	if (!Pool)
	{
		AddError(TEXT("Nothing pooled on the ramp"));
		return false;
	}

	// COINCIDENT SURFACES ARE THE SHIMMER. The floor is opaque and the water is
	// not, so where the puddle's underside lands in the same plane as the floor
	// the depth test has no answer -- and picks a different one per pixel and per
	// frame as the camera moves. The fix is that the underside is never in that
	// plane: it is below it, everywhere.
	const float Sink = 4.f;

	for (float X = -200.f; X <= 200.f; X += 25.f)
	{
		const FVector2D At(X, 0.f);

		float Floor = 0.f;
		if (!Fluids->FindGroundAt(At, 200.f, Floor, Pool))
		{
			continue;
		}

		// The drawn underside is the bed less the sink -- see GetUndersideSink.
		const float Underside = Pool->GetSurfaceBedAt(At) - Sink;

		TestTrue(FString::Printf(
			TEXT("The underside is buried at (%.0f, 0): %.2f against a floor at %.2f"),
			X, Underside, Floor), Underside < Floor);

		// AND NOT BURIED SO DEEP IT SHOWS. A sink big enough to clear the error is
		// also big enough to swallow the puddle if nobody bounds it, and the
		// waterline is what the player actually reads.
		TestTrue(FString::Printf(TEXT("But not sunk out of sight at (%.0f, 0)"), X),
			Pool->GetSurfaceLevelAt(At) > Floor);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPoolIsDrawnWhereItIsTest,
	"ARPG.World.Fluid.Deposit.APoolIsDrawnWhereItWasCast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPoolIsDrawnWhereItIsTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->MinimumArea = 100.f;
	Fluids->Definitions = { WaterDefinition };

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Stage = Scope.World->SpawnActor<AActor>(
		AActor::StaticClass(), FTransform::Identity, Params);

	// WELL AWAY FROM THE ORIGIN, which is the entire point of this case. A body
	// whose mesh is built in world coordinates but drawn as an actor's LOCAL ones
	// is displaced by however far from the origin it is -- so it looks fine in the
	// middle of a stage and flies off the map at the edges. Testing at the origin
	// is testing the one place the bug cannot show.
	const FVector2D Far(3000.f, -2000.f);

	UBoxComponent* Floor = NewObject<UBoxComponent>(Stage);
	Floor->SetupAttachment(Stage->GetRootComponent());
	Floor->RegisterComponent();
	Floor->SetBoxExtent(FVector(800.f, 800.f, 50.f));
	Floor->SetWorldLocation(FVector(Far.X, Far.Y, -50.f));
	Floor->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Floor->SetCollisionObjectType(ECC_WorldStatic);
	Floor->SetCollisionResponseToAllChannels(ECR_Block);

	AARPGFluidPool* Pool = Fluids->Deposit(FVector(Far.X, Far.Y, 0.f), 200.f, TAG_Element_Water);

	if (!Pool)
	{
		AddError(TEXT("Nothing pooled out at the edge of the stage"));
		return false;
	}

	// The outline is right -- it always was. The simulation knows exactly where
	// the puddle is; this is only ever about where it is DRAWN.
	const FVector2D RingCentre = ARPGFluidGeometry::PolygonCentroid(Pool->GetRing());
	TestEqual(TEXT("The outline is where it was cast, in X"),
		static_cast<float>(RingCentre.X), static_cast<float>(Far.X), 20.f);
	TestEqual(TEXT("And in Y"),
		static_cast<float>(RingCentre.Y), static_cast<float>(Far.Y), 20.f);

	// AND SO IS THE MESH. Taken in world space, through the component's own
	// transform, so it is the same question the player is asking: is the water
	// where the spell went off.
	UPrimitiveComponent* Drawn = Pool->GetSurfaceComponent();

	if (!Drawn)
	{
		AddError(TEXT("The pool has nothing drawn"));
		return false;
	}

	const FBoxSphereBounds Bounds = Drawn->CalcBounds(Drawn->GetComponentTransform());

	TestEqual(TEXT("The drawn water is where the outline is, in X"),
		static_cast<float>(Bounds.Origin.X), static_cast<float>(Far.X), 30.f);
	TestEqual(TEXT("And in Y"),
		static_cast<float>(Bounds.Origin.Y), static_cast<float>(Far.Y), 30.f);

	// And it is still the size it should be, so a mesh collapsed to nothing
	// cannot pass the two checks above by having no extent to be wrong about.
	TestTrue(FString::Printf(TEXT("And is the size of the puddle (%.0f)"), Bounds.BoxExtent.X),
		Bounds.BoxExtent.X > 100.0 && Bounds.BoxExtent.X < 400.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPoolsMergeWhenTheyMeetTest,
	"ARPG.World.Fluid.Deposit.OverlappingPuddlesAlwaysBecomeOne",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPoolsMergeWhenTheyMeetTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->MinimumArea = 100.f;
	WaterDefinition->EvaporationRate = 0.f;
	WaterDefinition->MergeDistance = 0.f;
	Fluids->Definitions = { WaterDefinition };

	// A FRESH PAIR EACH TIME. Depositing into a pool that is already growing hides
	// the bug being tested: the outline gets wider every round, so its bounding
	// box swallows the next centroid whatever the rule is. Each case has to start
	// from one puddle.
	auto Clear = [&]()
	{
		TArray<AARPGFluidPool*> Existing = Fluids->GetPools();
		for (AARPGFluidPool* Pool : Existing)
		{
			Fluids->RetireBody(Pool);
		}
	};

	// THE SHAPE OF THE OLD BUG, stated first because it is the one from the
	// screenshot: a SMALL puddle landing on the RIM of a big one. Its centre is
	// well outside the big one's bounding box -- so the old rule saw no merge --
	// while a third of it is lying on top. Two outlines crossing, each drawing
	// its own edge through the middle of the other.
	Clear();
	Fluids->Deposit(FVector::ZeroVector, 300.f, TAG_Element_Water);
	Fluids->Deposit(FVector(360.f, 0.f, 0.f), 80.f, TAG_Element_Water);

	TestEqual(TEXT("A small puddle on the rim of a big one joins it"),
		Fluids->GetPools().Num(), 1);

	// AND ACROSS THE WHOLE RANGE OF OVERLAPS, because the old rule was not wrong
	// everywhere -- it was wrong in a band, which is what made it look like
	// puddles merged sometimes and not others.
	const float Radius = 200.f;

	for (float Offset = 40.f; Offset < Radius * 2.f; Offset += 40.f)
	{
		Clear();
		Fluids->Deposit(FVector::ZeroVector, Radius, TAG_Element_Water);
		Fluids->Deposit(FVector(Offset, 0.f, 0.f), Radius, TAG_Element_Water);

		TestEqual(FString::Printf(
			TEXT("A puddle overlapping by %.0f is the same puddle"), Radius * 2.f - Offset),
			Fluids->GetPools().Num(), 1);
	}

	// AND ONE CLEAR OF IT IS NOT. A merge rule that swallowed everything would
	// pass every case above and be just as wrong.
	Clear();
	Fluids->Deposit(FVector::ZeroVector, Radius, TAG_Element_Water);
	Fluids->Deposit(FVector(Radius * 2.f + 100.f, 0.f, 0.f), Radius, TAG_Element_Water);

	TestEqual(TEXT("But a puddle well clear of it is its own"), Fluids->GetPools().Num(), 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPoolBridgesTwoTest,
	"ARPG.World.Fluid.Deposit.APuddleCastBetweenTwoJoinsBoth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPoolBridgesTwoTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->MinimumArea = 100.f;
	WaterDefinition->EvaporationRate = 0.f;
	WaterDefinition->MergeDistance = 0.f;
	Fluids->Definitions = { WaterDefinition };

	// Two puddles with a gap between them, so neither knows about the other.
	Fluids->Deposit(FVector(-300.f, 0.f, 0.f), 150.f, TAG_Element_Water);
	Fluids->Deposit(FVector(300.f, 0.f, 0.f), 150.f, TAG_Element_Water);

	TestEqual(TEXT("Two puddles to start with"), Fluids->GetPools().Num(), 2);

	// A THIRD ACROSS THE GAP, touching both. Joining only the first left the
	// second lying across the result as a separate body -- two outlines crossing,
	// each drawing its own rim through the middle of the other, which is the seam
	// running through the water in the report.
	Fluids->Deposit(FVector::ZeroVector, 250.f, TAG_Element_Water);

	TestEqual(TEXT("The one cast between them makes all three one"),
		Fluids->GetPools().Num(), 1);

	// AND IT IS THE WHOLE SHAPE, not the bridge with the ends dropped.
	const FBox2D Bounds = ARPGFluidGeometry::PolygonBounds(Fluids->GetPools()[0]->GetRing());

	TestTrue(FString::Printf(TEXT("Reaching the far end of the left one (%.0f)"), Bounds.Min.X),
		Bounds.Min.X < -400.f);
	TestTrue(FString::Printf(TEXT("And of the right one (%.0f)"), Bounds.Max.X),
		Bounds.Max.X > 400.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPoolIgnoresTheCasterTest,
	"ARPG.World.Fluid.Deposit.TheCasterStandingInItIsNotItsFloor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPoolIgnoresTheCasterTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFluidTestUtils;
	FTestWorld Scope;

	UARPGFluidSurfaceSubsystem* Fluids = Scope.World->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	UARPGMagicElement* Water = MakeElement(GetTransientPackage(), TAG_Element_Water);
	UARPGFluidDefinition* WaterDefinition = MakeWater(GetTransientPackage(), Water);
	WaterDefinition->MinimumArea = 100.f;
	Fluids->Definitions = { WaterDefinition };

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* Stage = Scope.World->SpawnActor<AActor>(
		AActor::StaticClass(), FTransform::Identity, Params);
	AddFloor(Stage, 0.f, 0.f, 800.f);

	// THE CASTER, STANDING WHERE THE SPELL GOES OFF. An emanation is cast AROUND
	// its caster, so this is not an edge case -- it is every single emanation.
	APawn* Caster = Scope.World->SpawnActor<APawn>(
		APawn::StaticClass(), FTransform(FVector(0.f, 0.f, 90.f)), Params);

	UCapsuleComponent* Body = NewObject<UCapsuleComponent>(Caster);
	Body->SetupAttachment(Caster->GetRootComponent());
	Body->RegisterComponent();
	Body->SetCapsuleSize(40.f, 90.f);
	Body->SetWorldLocation(FVector(0.f, 0.f, 90.f));
	Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Body->SetCollisionObjectType(ECC_Pawn);
	Body->SetCollisionResponseToAllChannels(ECR_Block);

	AARPGFluidPool* Pool = Fluids->Deposit(FVector::ZeroVector, 250.f, TAG_Element_Water);

	if (!Pool)
	{
		AddError(TEXT("Nothing pooled at the caster's feet"));
		return false;
	}

	// THE FLOOR IS THE FLOOR. A probe that stopped on the caster's capsule read
	// the ground as being at their shoulders, so the body was built to that
	// height -- its underside standing well proud of the real floor, and shifting
	// every time they moved. Which is the water thrashing about as it is placed.
	for (float X = -100.f; X <= 100.f; X += 50.f)
	{
		const FVector2D At(X, 0.f);

		TestEqual(FString::Printf(TEXT("The bed is the floor at (%.0f, 0)"), X),
			Pool->GetSurfaceBedAt(At), 0.f, 5.f);
	}

	TestEqual(TEXT("And the body sits on the floor, not on the caster"),
		static_cast<float>(Pool->GetActorLocation().Z), 0.f, 5.f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS
