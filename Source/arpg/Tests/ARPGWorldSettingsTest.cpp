// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "ARPGTestFixtures.h"

#if WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS

#include "ARPGFluidDefinition.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGSolidDefinition.h"
#include "ARPGSpreadDefinition.h"
#include "ARPGSpreadFuelMap.h"
#include "ARPGWorldSettings.h"
#include "PhysicalMaterials/PhysicalMaterial.h"

/**
 * The elemental solvers are actually configured with content.
 *
 * WHY THIS EXISTS. UARPGWorldSettings is what stops the four solvers running
 * inert, and its own header says so -- but nothing checked that the paths in it
 * resolve. Fire spread in particular shipped configured with NOTHING: there was
 * no SpreadDefinitions entry in DefaultGame.ini and no UARPGSpreadDefinition
 * asset anywhere in the project, so UARPGSpreadSubsystem::Initialize looped over
 * an empty list every session and simulated nothing.
 *
 * That failure is silent by design and reasonably so -- an empty list is a
 * legitimate world with no diffusive media, and warning about it would cry wolf
 * in every test fixture. The cost of that reasonable choice is that only a test
 * like this one can tell you the difference between "no media configured" and
 * "no media intended".
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGWorldSettingsResolveTest,
	"ARPG.World.Settings.SolverContentResolves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGWorldSettingsResolveTest::RunTest(const FString& Parameters)
{
	const UARPGWorldSettings& Settings = UARPGWorldSettings::Get();

	// --- The table every solver shares --------------------------------------
	TestNotNull(TEXT("The combination table resolves"),
		Settings.CombinationTable.LoadSynchronous());

	// --- Spread: the one that was configured with nothing at all ------------
	TestTrue(TEXT("At least one spread medium is configured"),
		Settings.SpreadDefinitions.Num() > 0);

	for (const TSoftObjectPtr<UARPGSpreadDefinition>& Soft : Settings.SpreadDefinitions)
	{
		UARPGSpreadDefinition* Definition = Soft.LoadSynchronous();
		if (!TestNotNull(*FString::Printf(TEXT("Spread definition '%s' resolves"), *Soft.ToString()),
			Definition))
		{
			continue;
		}

		// The subsystem rejects a definition with no element, with a warning, and
		// carries on -- so an unelemented medium is configured-but-inert, which
		// is the exact state this whole test exists to catch.
		TestNotNull(*FString::Printf(TEXT("'%s' names an element"), *Soft.ToString()),
			ToRawPtr(Definition->Element));

		TestTrue(*FString::Printf(TEXT("'%s' can actually spread"), *Soft.ToString()),
			Definition->SpreadRate > 0.f || Definition->GetObjectSpreadRate() > 0.f);
	}

	// --- Fluids and solids --------------------------------------------------
	TestTrue(TEXT("At least one fluid is configured"), Settings.FluidDefinitions.Num() > 0);
	for (const TSoftObjectPtr<UARPGFluidDefinition>& Soft : Settings.FluidDefinitions)
	{
		TestNotNull(*FString::Printf(TEXT("Fluid '%s' resolves"), *Soft.ToString()),
			Soft.LoadSynchronous());
	}

	TestTrue(TEXT("At least one solid is configured"), Settings.SolidDefinitions.Num() > 0);
	for (const TSoftObjectPtr<UARPGSolidDefinition>& Soft : Settings.SolidDefinitions)
	{
		TestNotNull(*FString::Printf(TEXT("Solid '%s' resolves"), *Soft.ToString()),
			Soft.LoadSynchronous());
	}

	return true;
}

/**
 * The placeholder fuel map expresses a firebreak, which is the whole point of it.
 *
 * A fuel map is optional -- an unbaked world is uniformly flammable, deliberately
 * -- so this is not asserting that one must exist. It asserts that the one
 * configured says something a uniform world could not: that some ground carries
 * fire and some refuses it.
 *
 * NAMED PHYSICAL SURFACES ARE PART OF THAT. With none named, every cell is
 * SurfaceType_Default and UARPGSpreadDefinition::SurfaceFuel could only ever
 * hold one row, making a firebreak unexpressable. The names live in
 * DefaultEngine.ini under [/Script/Engine.PhysicsSettings].
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFuelMapFirebreakTest,
	"ARPG.World.Settings.FuelMapExpressesAFirebreak",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFuelMapFirebreakTest::RunTest(const FString& Parameters)
{
	const UARPGWorldSettings& Settings = UARPGWorldSettings::Get();

	UARPGSpreadFuelMap* FuelMap = Settings.FuelMap.LoadSynchronous();
	if (!TestNotNull(TEXT("The fuel map resolves"), FuelMap))
	{
		return false;
	}

	TestTrue(TEXT("The origin chunk is baked"), FuelMap->HasChunk(FIntPoint(0, 0)));

	// The bake puts a stone stripe through columns 30-33 of a 64-wide chunk and
	// leaves the rest ordinary ground. Sampling either side of it is what proves
	// the map carries more than one surface -- a uniformly-baked map would pass
	// every other assertion here and still be worthless.
	const uint8 OnTheStripe  = FuelMap->GetSurfaceAt(FIntPoint(0, 0), 31.5f / 64.f, 0.5f);
	const uint8 OffTheStripe = FuelMap->GetSurfaceAt(FIntPoint(0, 0), 10.5f / 64.f, 0.5f);

	TestNotEqual(TEXT("The stripe is a different surface from the ground around it"),
		OnTheStripe, OffTheStripe);

	// And that the fire medium actually scores them differently. This is the
	// join between the two halves -- the ground saying what it is, and the
	// medium saying what that is worth -- and either half alone does nothing.
	for (const TSoftObjectPtr<UARPGSpreadDefinition>& Soft : Settings.SpreadDefinitions)
	{
		const UARPGSpreadDefinition* Definition = Soft.LoadSynchronous();
		if (!Definition || !Definition->bUsesField)
		{
			continue;
		}

		const float StripeFuel = Definition->GetSurfaceFuel(OnTheStripe);
		const float GroundFuel = Definition->GetSurfaceFuel(OffTheStripe);

		TestTrue(*FString::Printf(
			TEXT("'%s' scores the firebreak lower than open ground (%.2f vs %.2f)"),
			*Soft.ToString(), StripeFuel, GroundFuel),
			StripeFuel < GroundFuel);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS
