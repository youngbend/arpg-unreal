// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "ARPGTestFixtures.h"

#if WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS

#include "ARPGFluidField.h"
#include "ARPGFluidGeometry.h"

/**
 * The shallow-water field, on its own.
 *
 * NO WORLD, NO ACTORS, NO RENDERER, and that is the point of the whole file.
 * Every other fluid test in this project builds a UWorld, spawns a pool and
 * asserts on an actor, because the polygon model lived in one -- which made
 * every statement about the SOLVER an integration test that could fail for a
 * dozen reasons having nothing to do with the arithmetic. FARPGFluidField is a
 * plain class with an injected floor, so these are the real thing: construct,
 * pour, step, assert.
 *
 * THE FIRST TWO ARE THE ONES THAT MATTER. A fluid solver that loses mass is
 * wrong in a way no amount of tuning fixes and no screenshot reveals, and a
 * solver that will not route around an obstacle is the entire reason the polygon
 * model is being replaced. Everything below those two is detail.
 */

namespace
{
	/** Water: runs off anything, settles fast, no yield. */
	FARPGFluidFieldParams WaterParams()
	{
		FARPGFluidFieldParams Params;
		Params.FlowRate = 6.f;
		// MATCHES DA_Fluid_Water, and the reason it is not zero is written down
		// there. A fixture that configures water the game never ships tests a
		// solver nobody runs.
		Params.YieldSlope = 0.2f;
		Params.MinimumFilm = 0.05f;
		Params.WallSpeed = 200.f;
		Params.DepositDepth = 20.f;
		return Params;
	}

	/** Lava: slow, and with a yield slope it has to overcome before it moves. */
	FARPGFluidFieldParams LavaParams()
	{
		FARPGFluidFieldParams Params;
		Params.FlowRate = 0.75f;
		Params.YieldSlope = 2.f;
		Params.MinimumFilm = 0.4f;
		Params.WallSpeed = 12.f;
		Params.DepositDepth = 30.f;
		return Params;
	}

	/**
	 * A field over dead level ground.
	 *
	 * SMALL CHUNKS ON PURPOSE. 640cm at resolution 16 is the same 40cm cell the
	 * game runs, over a chunk an eighth the size -- so a puddle a couple of metres
	 * across crosses chunk edges, and every one of these tests exercises the
	 * cross-deposit path that a single-chunk fixture would never reach.
	 */
	void MakeFlatField(FARPGFluidField& Field, const FARPGFluidFieldParams& Params,
		float Height = 0.f)
	{
		Field.Configure(640.f, 16, Params);
		Field.SetBedProbe([Height](const FVector2D&, float& OutHeight)
			{
				OutHeight = Height;
				return true;
			});
	}
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFieldMassTest,
	"ARPG.World.FluidField.MassIsConserved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFieldMassTest::RunTest(const FString& Parameters)
{
	FARPGFluidField Field;
	MakeFlatField(Field, WaterParams());

	const double Poured = Field.PourVolume(FVector2D(0, 0), 0.f, 500000.0, 400.f);

	TestEqual(TEXT("A pour onto open ground places all of it"), Poured, 500000.0, 1.0);
	TestEqual(TEXT("And the field is holding it"), Field.GetTotalVolume(), 500000.0, 1.0);

	// A HUNDRED STEPS, which is five seconds at the field's rate and far longer
	// than any puddle takes to settle. If a single transfer anywhere fails to
	// balance, a hundred of them will show it.
	for (int32 Step = 0; Step < 100; ++Step)
	{
		Field.Step(0.05f);
	}

	// AN EQUALITY, NOT A TOLERANCE -- see FARPGFluidField::Step. Every transfer is
	// subtracted from one cell and added to another in the same pass, so the only
	// slack here is float addition over several thousand cells.
	TestEqual(TEXT("And still holding all of it after a hundred steps"),
		Field.GetTotalVolume(), 500000.0, 10.0);

	TestTrue(TEXT("Having spread out while doing it"), Field.GetWetCellCount() > 1);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFieldPuddleStopsTest,
	"ARPG.World.FluidField.APuddleStopsSpreading",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFieldPuddleStopsTest::RunTest(const FString& Parameters)
{
	// FLAT GROUND FOREVER, which is the case that has no natural edge. On a slope
	// or in a bowl the terrain stops the water; here nothing does except the fluid
	// itself, so this is where a missing yield stress shows up.
	FARPGFluidField Field;
	MakeFlatField(Field, WaterParams());

	Field.PourVolume(FVector2D(0, 0), 0.f, 500000.0, 400.f);

	for (int32 Step = 0; Step < 200; ++Step)
	{
		Field.Step(0.05f);
	}

	const double Settled = Field.GetWetAreaWithin(FVector2D(0, 0), 8192.f);

	// TEN SECONDS MORE, and it must not still be creeping. With YieldSlope at zero
	// this grew without limit -- half a cubic metre at MinimumFilm covers a
	// thousand square metres -- and the Niagara sheet drew every texel of its
	// window as water, which is what a puddle spreading past the horizon looks
	// like from inside it.
	for (int32 Step = 0; Step < 200; ++Step)
	{
		Field.Step(0.05f);
	}

	const double Later = Field.GetWetAreaWithin(FVector2D(0, 0), 8192.f);

	TestTrue(TEXT("The puddle has settled rather than kept creeping"),
		Later <= Settled * 1.05);

	// A HARD CEILING, because "settled" is not the same as "reasonable". The sheet
	// covers 2048cm, so a single pour spreading beyond a quarter of that has
	// already lost the argument visually whatever the solver thinks.
	TestTrue(TEXT("And it is a puddle rather than a flood"), Later < 1024.f * 1024.f);

	TestTrue(TEXT("And it still stands deep enough to be seen"),
		Field.SampleDepth(FVector2D(0, 0)) > 1.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFieldUpstairsVolumeTest,
	"ARPG.World.FluidField.WaterUpstairsStillCountsAsWaterNearby",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFieldUpstairsVolumeTest::RunTest(const FString& Parameters)
{
	// A BALCONY OVER A FLOOR, WITH WATER ON BOTH. Two surfaces at one XY is the
	// whole reason the field has layers, and only a fixture that fills both can
	// tell the difference between a query that reads every floor and one that
	// reads the first and stops.
	FARPGFluidField Field;
	Field.Configure(640.f, 16, WaterParams());

	Field.SetBedProbe([](const FVector2D&, float& OutHeight)
		{
			OutHeight = OutHeight > 200.f ? 400.f : 0.f;
			return true;
		});

	const double Downstairs = Field.PourVolume(FVector2D(0, 0), 0.f, 500000.0, 400.f);
	const double Upstairs = Field.PourVolume(FVector2D(0, 0), 400.f, 500000.0, 400.f);

	TestTrue(TEXT("Both pours landed"), Downstairs > 0.0 && Upstairs > 0.0);

	const double Both = Downstairs + Upstairs;

	TestEqual(TEXT("And the field is holding both"), Field.GetTotalVolume(), Both, 1.0);

	// THE ONE THAT REGRESSED. These walk a window rather than the whole field, and
	// they indexed the depth array with a CELL where it is keyed by SLOT -- a cell
	// on a layer -- so they reported the first floor and stopped. Half the water
	// in this fixture was invisible to them, and on a balcony with dry ground
	// underneath, all of it was.
	//
	// Nothing else noticed. Every reaction query found this water and the player
	// could stand in it. Only UARPGFluidPresentationSubsystem asks this, and only
	// to decide whether an element is worth drawing at all.
	TestEqual(TEXT("A window over it reports every floor's worth"),
		Field.GetVolumeWithin(FVector2D(0, 0), 2048.f), Both, 1.0);

	// AND STILL ANSWERS PER LAYER, because the argument was not merely unused --
	// it was accepted, documented and ignored. Which layer is which is allocation
	// order rather than height, so this asserts the split rather than the sides.
	const double First = Field.GetVolumeWithin(FVector2D(0, 0), 2048.f, 0);
	const double Second = Field.GetVolumeWithin(FVector2D(0, 0), 2048.f, 1);

	TestEqual(TEXT("The layers between them hold all of it"), First + Second, Both, 1.0);
	TestTrue(TEXT("And neither layer is holding it alone"), First > 0.0 && Second > 0.0);

	// Area counts a cell once however many of its floors are wet, so stacking a
	// second puddle exactly above the first must not double it.
	TestEqual(TEXT("Wet area is not doubled by the floor above"),
		Field.GetWetAreaWithin(FVector2D(0, 0), 2048.f),
		Field.GetWetAreaWithin(FVector2D(0, 0), 2048.f, 0), 1.0);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFieldAroundSolidTest,
	"ARPG.World.FluidField.FlowsAroundASolid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFieldAroundSolidTest::RunTest(const FString& Parameters)
{
	FARPGFluidField Field;
	MakeFlatField(Field, WaterParams());

	// A WALL WITH A GAP AT ONE END, and the wall has to be long enough that the
	// gap is the only way round -- the first version of this test ran it from
	// y = -200 and the water simply went round the SOUTH end, which is the solver
	// being right and the fixture being wrong. Open past y = 160 and closed for
	// twenty metres the other way.
	//
	// A polygon pool could not express this shape at all: it would need a hole,
	// and a hole that is open on one side is not a hole.
	const float Cell = Field.GetCellSize();
	double Displaced = 0.0;

	for (float Y = -2000.f; Y <= 160.f; Y += Cell)
	{
		Displaced += Field.MarkSolid(FVector2D(120.f, Y), 0.f, -10.f, 100.f);
	}

	TestEqual(TEXT("Nothing was displaced, because nothing was there yet"), Displaced, 0.0, 0.001);
	TestTrue(TEXT("The wall blocks its own cells"), Field.IsBlockedAt(FVector2D(120.f, 0.f)));
	TestFalse(TEXT("And stops where it stops"), Field.IsBlockedAt(FVector2D(120.f, 400.f)));

	Field.PourVolume(FVector2D(-40.f, 0.f), 0.f, 400000.0, 120.f);

	const double Before = Field.GetTotalVolume();

	for (int32 Step = 0; Step < 40; ++Step)
	{
		Field.Step(0.05f);
	}

	// STRAIGHT THROUGH IS THE FAILURE. A point directly east of the pour, behind
	// the middle of the wall, must stay dry however long this runs.
	TestFalse(TEXT("Water does not cross the wall"), Field.IsWetAt(FVector2D(200.f, 0.f)));
	TestFalse(TEXT("Nor stand inside it"), Field.IsWetAt(FVector2D(120.f, 0.f)));

	// AND ROUND THE END IS THE POINT. Given enough steps it reaches the far side
	// by the only route there is.
	for (int32 Step = 0; Step < 400; ++Step)
	{
		Field.Step(0.05f);
	}

	TestTrue(TEXT("But it does get round the open end"),
		Field.IsWetAt(FVector2D(200.f, 260.f)));
	TestFalse(TEXT("And never through the wall itself"),
		Field.IsWetAt(FVector2D(120.f, 0.f)));

	// IT ARRIVED BY THE GAP, and this is how you can tell. Once water is on the
	// east side it spreads south down the face and eventually reaches the shadow
	// legitimately -- so "the shadow is dry" is not the assertion, and an earlier
	// version of this test that made it was asserting that water cannot flow, not
	// that it cannot pass through rock. What the route shows up in is the
	// GRADIENT: deep beside the opening, thinning with every metre it has had to
	// travel back down the far side.
	const float NearTheGap = Field.SampleDepth(FVector2D(200.f, 200.f));
	const float DeepInTheShadow = Field.SampleDepth(FVector2D(200.f, -1200.f));

	TestTrue(TEXT("Deeper by the opening than far down the far side"),
		NearTheGap > DeepInTheShadow);

	TestEqual(TEXT("And none of it was lost going round"),
		Field.GetTotalVolume(), Before, 10.0);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFieldDownhillTest,
	"ARPG.World.FluidField.RunsDownhill",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFieldDownhillTest::RunTest(const FString& Parameters)
{
	FARPGFluidField Field;
	Field.Configure(640.f, 16, WaterParams());

	// A ramp falling away to the east: one centimetre down per centimetre across.
	Field.SetBedProbe([](const FVector2D& At, float& OutHeight)
		{
			OutHeight = -At.X;
			return true;
		});

	Field.PourVolume(FVector2D(-400.f, 0.f), -400.f, 200000.0, 100.f);

	for (int32 Step = 0; Step < 200; ++Step)
	{
		Field.Step(0.05f);
	}

	// A HEIGHTFIELD GETS THIS FOR NOTHING and the polygon model could not do it at
	// all: a pool was one outline at one height, and the best it ever managed was
	// draping a mesh over ground it had already decided the shape of.
	TestTrue(TEXT("Water has run downhill"), Field.IsWetAt(FVector2D(0.f, 0.f)));
	TestFalse(TEXT("And has not run uphill"), Field.IsWetAt(FVector2D(-1200.f, 0.f)));

	const FVector2D Flow = Field.SampleVelocity(FVector2D(-200.f, 0.f));
	TestTrue(TEXT("And it is moving the way it fell"), Flow.X > 0.0);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFieldYieldTest,
	"ARPG.World.FluidField.LavaStandsWhereWaterSheets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFieldYieldTest::RunTest(const FString& Parameters)
{
	// THE SAME POUR, THE SAME GROUND, TWO FLUIDS. What separates them is the yield
	// slope and nothing else, which is the claim UARPGFluidDefinition's comment
	// has been making since the runoff film was deleted and nothing has checked
	// since.
	FARPGFluidField Water;
	MakeFlatField(Water, WaterParams());
	Water.PourVolume(FVector2D::ZeroVector, 0.f, 300000.0, 80.f);

	FARPGFluidField Lava;
	MakeFlatField(Lava, LavaParams());
	Lava.PourVolume(FVector2D::ZeroVector, 0.f, 300000.0, 80.f);

	for (int32 Step = 0; Step < 200; ++Step)
	{
		Water.Step(0.05f);
		Lava.Step(0.05f);
	}

	TestTrue(TEXT("Water spreads further than lava"),
		Water.GetWetCellCount() > Lava.GetWetCellCount());

	// AND THAT FALLS OUT RATHER THAN BEING WRITTEN: a fluid that needs more head
	// before it will move necessarily stands deeper.
	TestTrue(TEXT("And lava therefore stands deeper"),
		Lava.SampleDepth(FVector2D::ZeroVector) > Water.SampleDepth(FVector2D::ZeroVector));

	TestEqual(TEXT("Neither lost any of it"), Water.GetTotalVolume(), 300000.0, 10.0);
	TestEqual(TEXT("Neither of them"), Lava.GetTotalVolume(), 300000.0, 10.0);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFieldRefloodTest,
	"ARPG.World.FluidField.ARegionCutOutOfTheMiddleRefloods",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFieldRefloodTest::RunTest(const FString& Parameters)
{
	FARPGFluidField Field;
	MakeFlatField(Field, WaterParams());

	Field.PourVolume(FVector2D::ZeroVector, 0.f, 600000.0, 300.f);

	for (int32 Step = 0; Step < 60; ++Step)
	{
		Field.Step(0.05f);
	}

	TestTrue(TEXT("The middle is wet to start with"), Field.IsWetAt(FVector2D::ZeroVector));

	const double Before = Field.GetTotalVolume();
	const TArray<FVector2D> Bite =
		ARPGFluidGeometry::MakeCircle(FVector2D::ZeroVector, 100.0, 16);

	const double Taken = Field.ConsumeRegion(Bite);

	TestTrue(TEXT("Freezing the middle takes something"), Taken > 0.0);
	TestEqual(TEXT("And the field is lighter by exactly that"),
		Field.GetTotalVolume(), Before - Taken, 1.0);
	TestFalse(TEXT("And the middle is dry the instant it happens"),
		Field.IsWetAt(FVector2D::ZeroVector));

	// THE STAMP HOLDS FOR ONE STEP, which is what stops the surrounding water
	// filling the hole back in before the ice has been built out of it.
	Field.Step(0.05f);
	TestFalse(TEXT("And is still dry through the step that made it"),
		Field.IsWetAt(FVector2D::ZeroVector));

	// AND THEN THE WATER COMES BACK, which is the behaviour AARPGSurfaceBody's own
	// comment says a liquid has and the polygon model could only fake by shrinking
	// the pool somewhere it had not been touched.
	for (int32 Step = 0; Step < 200; ++Step)
	{
		Field.Step(0.05f);
	}

	TestTrue(TEXT("Water flows back over the gap"), Field.IsWetAt(FVector2D::ZeroVector));

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFieldFloeTest,
	"ARPG.World.FluidField.WaterRunsUnderAFloatingSlab",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFieldFloeTest::RunTest(const FString& Parameters)
{
	FARPGFluidField Field;
	MakeFlatField(Field, WaterParams());

	// A SLAB WELL CLEAR OF THE FLOOR is a floe, not a plug, and the difference is
	// the whole of EARPGFluidCellFlags::Blocked. Its underside is at +30 over a
	// floor at 0, so there is water under it and you could swim beneath the edge.
	const float Cell = Field.GetCellSize();

	for (float Y = -160.f; Y <= 160.f; Y += Cell)
	{
		Field.MarkSolid(FVector2D(120.f, Y), 0.f, 30.f, 60.f);
	}

	TestFalse(TEXT("A floating slab blocks nothing"), Field.IsBlockedAt(FVector2D(120.f, 0.f)));

	Field.PourVolume(FVector2D(-40.f, 0.f), 0.f, 400000.0, 120.f);

	for (int32 Step = 0; Step < 300; ++Step)
	{
		Field.Step(0.05f);
	}

	TestTrue(TEXT("So the water runs straight under it"), Field.IsWetAt(FVector2D(200.f, 0.f)));

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFieldDisplacementTest,
	"ARPG.World.FluidField.ASlabDisplacesTheWaterItLandsIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFieldDisplacementTest::RunTest(const FString& Parameters)
{
	FARPGFluidField Field;
	MakeFlatField(Field, WaterParams());

	Field.PourVolume(FVector2D::ZeroVector, 0.f, 400000.0, 200.f);

	for (int32 Step = 0; Step < 40; ++Step)
	{
		Field.Step(0.05f);
	}

	const double Before = Field.GetTotalVolume();
	const float Cell = Field.GetCellSize();

	double Displaced = 0.0;

	for (float Y = -40.f; Y <= 40.f; Y += Cell)
	{
		for (float X = -40.f; X <= 40.f; X += Cell)
		{
			Displaced += Field.MarkSolid(FVector2D(X, Y), 0.f, -10.f, 100.f);
		}
	}

	// HANDED BACK RATHER THAN DELETED, which is why MarkSolid returns a volume at
	// all. A slab dropped into a puddle pushes water out of the way; the field
	// cannot decide where it goes without shoving it into ground the next cell of
	// the same slab is about to block, so the caller is told and puts it back.
	TestTrue(TEXT("Blocking wet ground displaces something"), Displaced > 0.0);
	TestEqual(TEXT("And the field is lighter by exactly that"),
		Field.GetTotalVolume(), Before - Displaced, 1.0);

	Field.PourVolume(FVector2D(0.f, 200.f), 0.f, Displaced, 200.f);

	TestEqual(TEXT("Putting it back restores the total"),
		Field.GetTotalVolume(), Before, 1.0);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFieldRegionTest,
	"ARPG.World.FluidField.TwoPoursThatCannotMeetAreTwoBodies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFieldRegionTest::RunTest(const FString& Parameters)
{
	FARPGFluidField Field;
	MakeFlatField(Field, WaterParams());

	Field.PourVolume(FVector2D(-2000.f, 0.f), 0.f, 100000.0, 100.f);
	Field.PourVolume(FVector2D(2000.f, 0.f), 0.f, 100000.0, 100.f);

	for (int32 Step = 0; Step < 40; ++Step)
	{
		Field.Step(0.05f);
	}

	TArray<FARPGFluidField::FRegion> Regions;
	Field.FindRegions(Regions);

	TestEqual(TEXT("Two pours forty metres apart are two bodies"), Regions.Num(), 2);

	if (Regions.Num() == 2)
	{
		TestEqual(TEXT("And between them they hold all of it"),
			Regions[0].Volume + Regions[1].Volume, Field.GetTotalVolume(), 1.0);

		// A region is a body, and a body has a place. What a pool actor used to be
		// spawned to carry, worked out from the cells instead.
		const double Separation =
			FMath::Abs(Regions[0].Centroid.X - Regions[1].Centroid.X);
		TestTrue(TEXT("And they are where they were poured"), Separation > 3000.0);
	}

	// AND ONE POUR BETWEEN THEM MAKES THEM ONE, which is what merging is now:
	// no MergeDistance, no polygon union, no decision. The cells touch or they
	// do not.
	// A BRIDGE HAS TO BE A BODY OF WATER, not a damp streak between two. Water
	// that stops spreading also stops holding a film open across twenty metres,
	// and a bridge thinner than the yield stress drains back into the two ends it
	// was meant to join -- which is right, and means the fixture needs a pour that
	// genuinely spans the gap rather than one that only technically touches.
	Field.PourVolume(FVector2D::ZeroVector, 0.f, 100000000.0, 2400.f);

	for (int32 Step = 0; Step < 200; ++Step)
	{
		Field.Step(0.05f);
	}

	Field.FindRegions(Regions);
	TestEqual(TEXT("Bridged, they are one body"), Regions.Num(), 1);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFieldWeatherTest,
	"ARPG.World.FluidField.WeatherIsTheOnlyThingThatChangesTheTotal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFieldWeatherTest::RunTest(const FString& Parameters)
{
	FARPGFluidField Field;
	MakeFlatField(Field, WaterParams());

	Field.PourVolume(FVector2D::ZeroVector, 0.f, 200000.0, 150.f);

	const double Start = Field.GetTotalVolume();

	// EVAPORATION IS ALLOWED TO LOSE MASS and the solver is not, which is the
	// entire reason these are two functions. Folding the weather into Step would
	// make the conservation test above impossible to write.
	const double Lost = Field.ApplyWeather(-1.f, 1.f);

	TestTrue(TEXT("Evaporation takes something"), Lost < 0.0);
	TestEqual(TEXT("And the total moved by exactly that"),
		Field.GetTotalVolume(), Start + Lost, 1.0);

	const double Gained = Field.ApplyWeather(1.f, 1.f);
	TestTrue(TEXT("Rain puts something back"), Gained > 0.0);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFieldLedgeTest,
	"ARPG.World.FluidField.WaterStopsWhereTheFloorDoes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFieldLedgeTest::RunTest(const FString& Parameters)
{
	FARPGFluidField Field;
	Field.Configure(640.f, 16, WaterParams());

	// A PLATFORM WITH NOTHING PAST ITS EAST EDGE. The probe REFUSES rather than
	// answering with a low number, which is the difference between a cliff and a
	// drop: there is no floor there at all.
	Field.SetBedProbe([](const FVector2D& At, float& OutHeight)
		{
			if (At.X > 300.f)
			{
				return false;
			}

			OutHeight = 0.f;
			return true;
		});

	Field.PourVolume(FVector2D::ZeroVector, 0.f, 800000.0, 250.f);

	const double Before = Field.GetTotalVolume();

	for (int32 Step = 0; Step < 300; ++Step)
	{
		Field.Step(0.05f);
	}

	TestFalse(TEXT("Water does not run off the end of the world"),
		Field.IsWetAt(FVector2D(400.f, 0.f)));

	// AND IT IS NOT LOST EITHER, which is the failure mode a naive "pour it over
	// the edge and forget it" would have: the total would fall and nothing would
	// say where it went.
	TestEqual(TEXT("It piles up against the lip instead"),
		Field.GetTotalVolume(), Before, 10.0);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFieldDeterminismTest,
	"ARPG.World.FluidField.TheSameRunTwiceIsTheSameRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFieldDeterminismTest::RunTest(const FString& Parameters)
{
	auto Run = [](double& OutVolume, int32& OutCells, float& OutDepth)
		{
			FARPGFluidField Field;
			MakeFlatField(Field, WaterParams());

			Field.PourVolume(FVector2D(-100.f, 40.f), 0.f, 250000.0, 150.f);
			Field.PourVolume(FVector2D(180.f, -60.f), 0.f, 150000.0, 90.f);

			for (int32 Step = 0; Step < 120; ++Step)
			{
				Field.Step(0.05f);
			}

			OutVolume = Field.GetTotalVolume();
			OutCells = Field.GetWetCellCount();
			OutDepth = Field.SampleDepth(FVector2D(20.f, 0.f));
		};

	double VolumeA = 0.0, VolumeB = 0.0;
	int32 CellsA = 0, CellsB = 0;
	float DepthA = 0.f, DepthB = 0.f;

	Run(VolumeA, CellsA, DepthA);
	Run(VolumeB, CellsB, DepthB);

	// EXACT, NOT CLOSE. The solver is a Jacobi update over a map whose iteration
	// order is the only thing that could vary between two runs, and if that order
	// can change the answer then the sweep is reading water it has already moved
	// -- which is the bug the per-chunk deltas exist to make impossible.
	TestEqual(TEXT("Volume is identical"), VolumeA, VolumeB, 0.0);
	TestEqual(TEXT("Wet cell count is identical"), CellsA, CellsB);
	TestEqual(TEXT("And so is the depth anywhere in it"), DepthA, DepthB, 0.f);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFieldOutlineTest,
	"ARPG.World.FluidField.ARegionCanHandBackAnOutline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFieldOutlineTest::RunTest(const FString& Parameters)
{
	FARPGFluidField Field;
	MakeFlatField(Field, WaterParams());

	Field.PourVolume(FVector2D::ZeroVector, 0.f, 600000.0, 300.f);

	for (int32 Step = 0; Step < 60; ++Step)
	{
		Field.Step(0.05f);
	}

	TArray<FARPGFluidField::FRegion> Regions;
	Field.FindRegions(Regions);

	if (!TestEqual(TEXT("One pour is one body"), Regions.Num(), 1))
	{
		return false;
	}

	TArray<FVector2D> Ring;
	Field.BuildRegionOutline(Regions[0], Ring);

	TestTrue(TEXT("Which has an outline"), Ring.Num() >= 4);

	// THE OUTLINE AND THE CELLS MUST AGREE, and that is the whole contract. This
	// ring is handed to IntersectWithHoles when a spell freezes part of the body,
	// so a shape that disagreed with IsWetAt about its own border would freeze
	// water that is not there or miss water that is.
	const double RingArea = ARPGFluidGeometry::PolygonArea(Ring);
	const double CellArea = Regions[0].Area;

	TestEqual(TEXT("And its area is the area of the cells in it"), RingArea, CellArea, 1.0);

	TestTrue(TEXT("The middle of the body is inside its outline"),
		ARPGFluidGeometry::PolygonContains(Ring, Regions[0].Centroid));
	TestFalse(TEXT("And a point well outside is not"),
		ARPGFluidGeometry::PolygonContains(Ring, FVector2D(100000.0, 0.0)));

	// COLLAPSED, or a two-metre straight bank is five vertices saying the same
	// thing and every boolean downstream pays for all five.
	TestTrue(TEXT("Collinear runs were collapsed"), Ring.Num() < Regions[0].Cells.Num());

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFieldOutlineAroundSolidTest,
	"ARPG.World.FluidField.AnOutlineClosesOverAPillarStandingInIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFieldOutlineAroundSolidTest::RunTest(const FString& Parameters)
{
	FARPGFluidField Field;
	MakeFlatField(Field, WaterParams());

	// A PILLAR IN THE MIDDLE OF A PUDDLE -- the shape that has no polygon
	// representation at all under the old model, because a fluid carried no hole
	// and a mesh could carry at most one.
	const float Cell = Field.GetCellSize();

	for (float Y = -60.f; Y <= 60.f; Y += Cell)
	{
		for (float X = -60.f; X <= 60.f; X += Cell)
		{
			Field.MarkSolid(FVector2D(X, Y), 0.f, -10.f, 100.f);
		}
	}

	// ENOUGH TO REACH THE FAR SIDE. What is under test is the SHAPE -- water gets
	// all the way round a pillar and the outline closes over it -- and water with
	// a yield stress only travels as far as its own head carries it, so the pour
	// has to be big enough to arrive. Eight metres from here to the far sampling
	// point; this covers it with room to spare.
	Field.PourVolume(FVector2D(0.f, 400.f), 0.f, 5000000.0, 600.f);

	for (int32 Step = 0; Step < 300; ++Step)
	{
		Field.Step(0.05f);
	}

	TestFalse(TEXT("No water stands in the pillar"), Field.IsWetAt(FVector2D::ZeroVector));
	TestTrue(TEXT("But it is wet all the way round"),
		Field.IsWetAt(FVector2D(0.f, -400.f)) && Field.IsWetAt(FVector2D(400.f, 0.f)));

	TArray<FARPGFluidField::FRegion> Regions;
	Field.FindRegions(Regions);

	TestEqual(TEXT("Water round a pillar is still one body"), Regions.Num(), 1);

	if (Regions.Num() != 1)
	{
		return false;
	}

	TArray<FVector2D> Ring;
	Field.BuildRegionOutline(Regions[0], Ring);

	// AND THE OUTLINE IS THE OUTER ONE. The tracer finds the loop round the
	// pillar as well, and a fluid has never carried a hole -- water flows back
	// over a gap, and this gap is a blocked cell the solver already refuses.
	TestTrue(TEXT("The outline encloses the pillar it flowed around"),
		ARPGFluidGeometry::PolygonContains(Ring, FVector2D::ZeroVector));

	// WHICH MEANS RING AREA IS BIGGER THAN CELL AREA, by exactly the pillar. That
	// is not a defect: it is the documented difference between what a liquid looks
	// like and what it occupies, and the reason IsSurfaceAt asks the cells.
	TestTrue(TEXT("So the ring is larger than the water in it"),
		ARPGFluidGeometry::PolygonArea(Ring) > Regions[0].Area);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFluidFieldWindowTest,
	"ARPG.World.FluidField.TheWindowHandedToTheGpuSaysWhatTheFieldSays",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFluidFieldWindowTest::RunTest(const FString& Parameters)
{
	FARPGFluidField Field;
	MakeFlatField(Field, WaterParams());

	// A wall down one side, so the picture has all four channels to say something
	// about: wet ground, dry ground, a floor, and something standing on it.
	const float Cell = Field.GetCellSize();

	for (float Y = -400.f; Y <= 400.f; Y += Cell)
	{
		Field.MarkSolid(FVector2D(200.f, Y), 0.f, -10.f, 90.f);
	}

	Field.PourVolume(FVector2D(-100.f, 0.f), 0.f, 400000.0, 200.f);

	for (int32 Step = 0; Step < 40; ++Step)
	{
		Field.Step(0.05f);
	}

	FARPGFluidField::FWindow Window;
	Window.Origin = FVector2D(-640.f, -640.f);
	Window.Extent = 1280.f;
	Window.Resolution = 64;
	Window.NearZ = 0.f;

	TArray<FFloat16Color> Pixels;
	Field.SampleWindow(Window, Pixels);

	TestEqual(TEXT("The picture is the size it was asked for"), Pixels.Num(), 64 * 64);

	// EVERY TEXEL AGREES WITH THE FIELD IT IS A PICTURE OF, which is the whole
	// contract. A shader reading a texture that disagrees with the simulation is
	// the one failure this architecture exists to make impossible, and it is
	// invisible in a screenshot -- the water simply looks slightly wrong.
	const float Step = Window.Extent / Window.Resolution;

	int32 Wet = 0;
	int32 Raised = 0;
	int32 Mismatched = 0;
	int32 Borrowed = 0;

	for (int32 Y = 0; Y < Window.Resolution; ++Y)
	{
		for (int32 X = 0; X < Window.Resolution; ++X)
		{
			const FVector2D At(
				Window.Origin.X + (X + 0.5f) * Step,
				Window.Origin.Y + (Y + 0.5f) * Step);

			const FLinearColor Texel = Pixels[Y * Window.Resolution + X].GetFloats();

			// Half a millimetre, which is float16 across the range these carry.
			// Blocked texels are written dry outright -- see SampleWindow.
			const float Expected = Field.IsBlockedAt(At) ? 0.f : Field.SampleDepth(At);

			if (!FMath::IsNearlyEqual(Texel.R, Expected, 0.05f))
			{
				++Mismatched;
			}

			// THE FLOOR, EXCEPT WHERE THE FIELD HAS NO FLOOR TO GIVE.
			//
			// A texel over ground the field has never had water on gets its floor
			// DILATED IN from a neighbour that does know -- see the tail of
			// SampleWindow. The material samples this bilinearly, so without it
			// every texel on the dry side of a rim blends the viewer's own Z into
			// the wet one beside it, and the surface tears along the whole
			// boundary: measured at 180 such pairs with floors three metres apart.
			//
			// JUDGED BY THE TEXEL'S OWN CONTENT, not by asking the field a second
			// question. Where the picture says there is water or a solid, its floor
			// has to be the field's exactly. Where it says the texel is dry, the
			// floor is allowed to be a borrowed one -- and being dry is precisely
			// what keeps the borrowed value off the screen.
			if (Texel.R > 0.f || Texel.A >= 0.5f)
			{
				if (!FMath::IsNearlyEqual(Texel.G, Field.SampleBed(At), 0.05f))
				{
					++Mismatched;
				}
			}
			else if (!FMath::IsNearlyEqual(Texel.G, Field.SampleBed(At), 0.05f))
			{
				++Borrowed;
			}

			if (Texel.R > 0.f)
			{
				++Wet;
			}

			// A IS BLOCKED AND NOTHING ELSE -- see SampleWindow. And B rises above G
			// only where something GROUNDED stands: a floe leaves the bottom contour
			// alone, because water runs underneath one.
			const bool bBlocked = Texel.A >= 0.5f;
			const bool bRaised = Texel.B > Texel.G + 0.001f;

			if (bRaised)
			{
				++Raised;
			}

			// RAISED IMPLIES BLOCKED. If the contour ever rises where fluid is still
			// allowed, a floe has started damming the sheet.
			if (bRaised && !bBlocked)
			{
				++Mismatched;
			}

			if (bBlocked != Field.IsBlockedAt(At))
			{
				++Mismatched;
			}

			// THE BOTTOM CONTOUR IS THE FLOOR OR THE THING ON IT, whichever is
			// higher -- which is what makes water part around a wall in the sheet
			// without anything in the graph knowing what a wall is.
			if (bBlocked && Texel.B < Texel.G)
			{
				++Mismatched;
			}
		}
	}

	TestEqual(TEXT("No texel disagrees with the field"), Mismatched, 0);
	TestTrue(TEXT("The picture has water in it"), Wet > 0);
	TestTrue(TEXT("And knows about the wall"), Raised > 0);

	// AND THE DILATION STAYS SMALL. It reaches one texel, which is how far a
	// bilinear tap reaches; unbounded it would march the floor outward across the
	// whole window a texel per pass, which is a different picture entirely.
	//
	// NO LOWER BOUND HERE ON PURPOSE: this fixture is dead level, so a borrowed
	// floor and the real one are the same number and the dilation is invisible to
	// it. What it can still catch is the dilation running away.
	TestTrue(TEXT("Borrowed floors stay a rim's worth"), Borrowed <= Wet);

	// AND THE WALL IS NOT DRAWN AS WATER. A texel inside the wall has to read dry
	// however deep the puddle beside it is.
	const int32 Middle = Window.Resolution / 2;
	const int32 WallX = FMath::FloorToInt((200.f - Window.Origin.X) / Step);

	if (Pixels.IsValidIndex(Middle * Window.Resolution + WallX))
	{
		const FLinearColor Wall = Pixels[Middle * Window.Resolution + WallX].GetFloats();

		TestEqual(TEXT("The wall reads dry"), Wall.R, 0.f, 0.001f);
		TestTrue(TEXT("And stands above its own floor"), Wall.B > Wall.G);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS
