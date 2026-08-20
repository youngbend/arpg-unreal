// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "ARPGTestFixtures.h"

#if WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS

#include "ARPGConductionSubsystem.h"
#include "ARPGDischargeContext.h"
#include "ARPGDischargeEffect.h"
#include "ARPGElementalReactionSubsystem.h"
#include "ARPGElementPalette.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGGameplayTags.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicElement.h"
#include "Components/SphereComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/Actor.h"

#include <initializer_list>

/**
 * Elemental reactions and conduction.
 *
 * THE ENERGY MODEL IS THE POINT. Every case below drives Resolve directly rather
 * than staging a physics overlap: the model -- who is spent, who survives, how
 * big the release is -- is what the three interesting behaviours fall out of,
 * and it is independently true of whether the broadphase fired. The overlap path
 * is one call into the same function.
 *
 * Product SPAWNING is not asserted, only the decision to spawn and where. A
 * product is an effect actor needing authored content; what is testable without
 * it is which element resolved and at what point, and "the steam appeared in the
 * wrong place" is a fault with no other symptom.
 */
namespace ARPGReactionTestUtils
{
	// This suite has always begun play through AWorldSettings.
	using FTestWorld = ARPGTest::FTestWorldBegunPlay;

	UARPGMagicElement* MakeElement(UObject* Outer, FGameplayTag Tag)
	{
		UARPGMagicElement* Element = NewObject<UARPGMagicElement>(Outer);
		Element->ElementTag = Tag;
		return Element;
	}

	/**
	 * An actor carrying a volume, with a real sphere collider so overlap-driven
	 * paths (conduction's graph walk, ambient exposure) work as they do in game.
	 */
	UARPGElementalVolumeComponent* SpawnVolume(UWorld* World, UARPGMagicElement* Element,
		float Energy, const FVector& Location, float Radius = 100.f)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(),
			FTransform(Location), Params);

		USphereComponent* Sphere = NewObject<USphereComponent>(Actor);
		Sphere->SetSphereRadius(Radius);

		// Movable, or the engine skips overlap updates for it entirely and the
		// conduction graph sees an empty world -- see the volume component.
		Sphere->SetMobility(EComponentMobility::Movable);
		Sphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Sphere->SetCollisionObjectType(ECC_WorldDynamic);
		Sphere->SetCollisionResponseToAllChannels(ECR_Overlap);
		Sphere->SetGenerateOverlapEvents(true);
		Actor->SetRootComponent(Sphere);
		Sphere->RegisterComponent();

		// Positioned AFTER registering, and explicitly: a bare AActor has no root
		// at spawn time, so the spawn transform never reaches the component that
		// becomes one. Every geometric assertion below depends on this.
		Sphere->SetWorldLocation(Location);

		UARPGElementalVolumeComponent* Volume = NewObject<UARPGElementalVolumeComponent>(Actor);
		Volume->Element = Element;
		Volume->OverlapSource = Sphere;
		Volume->SetupAttachment(Sphere);
		Volume->RegisterComponent();
		Volume->SetEnergy(Energy);

		return Volume;
	}

	/**
	 * Forces the overlap lists to resolve.
	 *
	 * A bare test world never ticks physics, so GetOverlappingComponents returns
	 * the cached list -- which is empty until something asks for it to be built.
	 * In game the physics scene does this every frame.
	 */
	void ResolveOverlaps(std::initializer_list<UARPGElementalVolumeComponent*> Volumes)
	{
		for (UARPGElementalVolumeComponent* Volume : Volumes)
		{
			if (Volume && Volume->OverlapSource)
			{
				Volume->OverlapSource->UpdateOverlaps();
			}
		}
	}

	UARPGMagicCombinationEntry* MakeEntry(UObject* Outer, FGameplayTag A, FGameplayTag B,
		UARPGMagicElement* Result)
	{
		UARPGMagicCombinationEntry* Entry = NewObject<UARPGMagicCombinationEntry>(Outer);
		Entry->RequiredElements.AddTag(A);
		Entry->RequiredElements.AddTag(B);
		Entry->Result = Result;
		Entry->Scope = 0xF;
		return Entry;
	}

	struct FRig
	{
		UARPGElementalReactionSubsystem* Reactions = nullptr;
		UARPGMagicCombinationTable* Table = nullptr;
		UARPGMagicElement* Fire = nullptr;
		UARPGMagicElement* Water = nullptr;
		UARPGMagicElement* Steam = nullptr;
	};

	FRig BuildRig(UWorld* World)
	{
		FRig Rig;
		Rig.Reactions = World->GetSubsystem<UARPGElementalReactionSubsystem>();

		Rig.Table = NewObject<UARPGMagicCombinationTable>();
		Rig.Fire = MakeElement(Rig.Table, TAG_Element_Fire);
		Rig.Water = MakeElement(Rig.Table, TAG_Element_Water);
		Rig.Steam = MakeElement(Rig.Table, TAG_Element_Steam);

		// A palette, so the product resolves to the shared placeholder instead of
		// warning that it would be invisible. The warning is correct -- these
		// tests do genuinely spawn products -- and silencing it here keeps it
		// meaningful when it fires for real content.
		Rig.Steam->Palette = NewObject<UARPGElementPalette>(Rig.Table);
		Rig.Table->Entries.Add(MakeEntry(Rig.Table, TAG_Element_Fire, TAG_Element_Water, Rig.Steam));

		if (Rig.Reactions)
		{
			Rig.Reactions->CombinationTable = Rig.Table;
			Rig.Reactions->MinMagnitude = 0.f;
		}
		return Rig;
	}
}

// ---------------------------------------------------------------------------
// Reaching the solver at all
//
// Everything below this asserts the energy model, and all of it was true and
// unreachable: a spawned spell carried no elemental volume and, worse, no
// primitive collider of any kind -- the hitbox is a scene component that sweeps
// by hand. Nothing could ever meet anything, so no cast spell ever reacted.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGReactionCastSpellTest,
	"ARPG.World.Reaction.ACastSpellIsMadeOfItsElement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGReactionCastSpellTest::RunTest(const FString& Parameters)
{
	using namespace ARPGReactionTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* Caster = Scope.World->SpawnActor<AActor>(AActor::StaticClass(),
		FTransform::Identity, Params);

	// A spell as the discharge ability actually spawns one: deferred, context
	// stamped, then finished. The context is the ONLY thing that energises it.
	auto CastSpell = [&](UARPGMagicElement* Element, float Damage, float Radius, FVector At)
	{
		AARPGDischargeEffect* Effect = Scope.World->SpawnActorDeferred<AARPGDischargeEffect>(
			AARPGDischargeEffect::StaticClass(), FTransform(At), Caster);

		FARPGDischargeContext Context;
		Context.PrimaryElement = Element;
		Context.Caster = Caster;
		Context.ComputedDamage = Damage;

		Effect->ReactionRadius = Radius;
		Effect->InitializeFromContext(Context);
		Effect->FinishSpawning(FTransform(At));

		return Effect;
	};

	AARPGDischargeEffect* Fireball = CastSpell(Rig.Fire, 40.f, 100.f, FVector(0, 0, 0));

	TestNotNull(TEXT("A cast spell has a collider to meet things with"), Fireball->Collider.Get());
	TestNotNull(TEXT("And a volume saying what it is made of"), Fireball->Volume.Get());
	TestSamePtr(TEXT("Made of the element that was cast"),
		Fireball->Volume->Element.Get(), Rig.Fire);

	// THE SAME NUMBER AS THE DAMAGE. ComputedDamage is precomputed on the context
	// precisely so a reaction accounts for charge, mastery and buffs without
	// re-deriving any of them -- a charged fireball out-trades a tapped one.
	TestEqual(TEXT("Energised from its own computed damage"),
		Fireball->Volume->GetEnergy(), 40.f);
	TestSamePtr(TEXT("And attributes to the caster, not to itself"),
		Fireball->Volume->SourceActor.Get(), Caster);

	TestNotEqual(TEXT("Its collider is live"),
		Fireball->Collider->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
	TestEqual(TEXT("At the radius the spell asked for"),
		Fireball->Collider->GetUnscaledSphereRadius(), 100.f);

	// OVERLAP, NEVER BLOCK. A collider that blocked would have the fireball
	// bouncing off the water it is supposed to react with.
	TestEqual(TEXT("And overlaps rather than blocking"),
		Fireball->Collider->GetCollisionResponseToChannel(ECC_WorldDynamic), ECR_Overlap);

	// End to end, minus the broadphase: two spawned spells, resolved the same way
	// an overlap would resolve them.
	AARPGDischargeEffect* Jet = CastSpell(Rig.Water, 40.f, 100.f, FVector(50, 0, 0));

	Rig.Reactions->Resolve(Fireball->Volume, Jet->Volume);

	TestSamePtr(TEXT("Fire meeting water makes steam"),
		Rig.Reactions->GetLastProduct(), Rig.Steam);
	TestEqual(TEXT("And both spells are spent"), Fireball->Volume->GetEnergy(), 0.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGReactionInertSpellTest,
	"ARPG.World.Reaction.ASpellWithNoReactionRadiusStaysInert",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGReactionInertSpellTest::RunTest(const FString& Parameters)
{
	using namespace ARPGReactionTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AARPGDischargeEffect* Effect = Scope.World->SpawnActorDeferred<AARPGDischargeEffect>(
		AARPGDischargeEffect::StaticClass(), FTransform::Identity, nullptr);

	FARPGDischargeContext Context;
	Context.PrimaryElement = Rig.Fire;
	Context.ComputedDamage = 40.f;

	// ReactionRadius left at its default of 0.
	Effect->InitializeFromContext(Context);
	Effect->FinishSpawning(FTransform::Identity);

	// Most spells never react, and paying overlap traffic on every one of them to
	// discover that is the cost this default avoids.
	TestEqual(TEXT("A spell with no reaction radius keeps its collider off"),
		Effect->Collider->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
	TestNull(TEXT("And is made of nothing"), Effect->Volume->Element.Get());

	return true;
}

// ---------------------------------------------------------------------------
// The energy model
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGReactionMatchedTest,
	"ARPG.World.Reaction.MatchedProjectilesBothSpend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGReactionMatchedTest::RunTest(const FString& Parameters)
{
	using namespace ARPGReactionTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	if (!Rig.Reactions)
	{
		AddError(TEXT("Setup: no reaction subsystem in the test world."));
		return false;
	}

	UARPGElementalVolumeComponent* Fireball =
		SpawnVolume(Scope.World, Rig.Fire, 40.f, FVector(0, 0, 0));
	UARPGElementalVolumeComponent* Jet =
		SpawnVolume(Scope.World, Rig.Water, 40.f, FVector(100, 0, 0));

	Rig.Reactions->Resolve(Fireball, Jet);

	TestEqual(TEXT("Equal energies spend both sides"), Fireball->GetEnergy(), 0.f);
	TestEqual(TEXT("Both, not just one"), Jet->GetEnergy(), 0.f);
	TestSamePtr(TEXT("Fire and water produce steam"), Rig.Reactions->GetLastProduct(), Rig.Steam);

	// Midpoint, because neither is a reservoir -- see the contact-point test.
	TestEqual(TEXT("Contact is between them"),
		static_cast<float>(Rig.Reactions->GetLastContactPoint().X), 50.f, 1.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGReactionLopsidedTest,
	"ARPG.World.Reaction.SurplusSurvivesAtReducedPower",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGReactionLopsidedTest::RunTest(const FString& Parameters)
{
	using namespace ARPGReactionTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UARPGElementalVolumeComponent* BigFire =
		SpawnVolume(Scope.World, Rig.Fire, 100.f, FVector(0, 0, 0));
	UARPGElementalVolumeComponent* SmallWater =
		SpawnVolume(Scope.World, Rig.Water, 30.f, FVector(100, 0, 0));

	Rig.Reactions->Resolve(BigFire, SmallWater);

	// Reacted is the SMALLER of the two, so the water is spent and the fire
	// carries on with its surplus. No case analysis produced this -- it falls
	// out of min().
	TestEqual(TEXT("The smaller side is spent"), SmallWater->GetEnergy(), 0.f);
	TestEqual(TEXT("The larger keeps its surplus"), BigFire->GetEnergy(), 70.f);

	// And that surplus is what the effect scales itself by.
	TestEqual(TEXT("Reported as a fraction of its original"),
		BigFire->GetEnergy() / BigFire->GetInitialEnergy(), 0.7f, 0.01f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGReactionReservoirTest,
	"ARPG.World.Reaction.ReservoirIsNotDepleted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGReactionReservoirTest::RunTest(const FString& Parameters)
{
	using namespace ARPGReactionTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UARPGElementalVolumeComponent* Fireball =
		SpawnVolume(Scope.World, Rig.Fire, 40.f, FVector(0, 0, 0), 50.f);

	// A river: bottomless, damping, and a tall box whose waterline is well below
	// its top. Placed so the fireball is genuinely at the surface.
	UARPGElementalVolumeComponent* River =
		SpawnVolume(Scope.World, Rig.Water, 10000.f, FVector(0, 0, 0), 500.f);
	River->bReservoir = true;
	River->Absorption = 0.9f;
	River->SurfaceHeightOffset = 0.f;

	Rig.Reactions->Resolve(Fireball, River);

	TestEqual(TEXT("The projectile is fully spent"), Fireball->GetEnergy(), 0.f);
	TestEqual(TEXT("A river does not run out"), River->GetEnergy(), 10000.f);
	TestSamePtr(TEXT("It still makes steam"), Rig.Reactions->GetLastProduct(), Rig.Steam);

	// Anchored on the PROJECTILE and put on the waterline -- a river's own
	// origin is the centre of a huge box, tens of metres from where the fireball
	// actually touched.
	TestEqual(TEXT("Contact is on the waterline, not the box centre"),
		static_cast<float>(Rig.Reactions->GetLastContactPoint().Z),
		River->GetSurfaceHeightAt(FVector::ZeroVector), 1.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGReactionAboveWaterTest,
	"ARPG.World.Reaction.NothingReactsAboveTheWaterline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGReactionAboveWaterTest::RunTest(const FString& Parameters)
{
	using namespace ARPGReactionTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	// A fireball still well above the surface, inside the tall broadphase box.
	UARPGElementalVolumeComponent* Fireball =
		SpawnVolume(Scope.World, Rig.Fire, 40.f, FVector(0, 0, 400), 20.f);

	UARPGElementalVolumeComponent* River =
		SpawnVolume(Scope.World, Rig.Water, 10000.f, FVector(0, 0, 0), 500.f);
	River->bReservoir = true;
	River->SurfaceHeightOffset = 0.f;

	Rig.Reactions->Resolve(Fireball, River);

	// The regression this exists for: the overlap fires while the projectile is
	// still metres in the air, and reacting there detonated it mid-flight and
	// snapped the splash down to a surface it had not touched.
	TestEqual(TEXT("A fireball flying over a river does not detonate"),
		Fireball->GetEnergy(), 40.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGReactionSameElementTest,
	"ARPG.World.Reaction.SameElementDoesNotReact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGReactionSameElementTest::RunTest(const FString& Parameters)
{
	using namespace ARPGReactionTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UARPGElementalVolumeComponent* First =
		SpawnVolume(Scope.World, Rig.Fire, 40.f, FVector(0, 0, 0));
	UARPGElementalVolumeComponent* Second =
		SpawnVolume(Scope.World, Rig.Fire, 40.f, FVector(100, 0, 0));

	Rig.Reactions->Resolve(First, Second);

	// Two fireballs meeting should merge or pass through, not annihilate.
	TestEqual(TEXT("Neither is consumed"), First->GetEnergy(), 40.f);
	TestEqual(TEXT("By the other"), Second->GetEnergy(), 40.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGReactionNeutraliseTest,
	"ARPG.World.Reaction.NoRecipeStillNeutralises",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGReactionNeutraliseTest::RunTest(const FString& Parameters)
{
	using namespace ARPGReactionTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UARPGMagicElement* Earth = MakeElement(Rig.Table, TAG_Element_Earth);

	UARPGElementalVolumeComponent* Fireball =
		SpawnVolume(Scope.World, Rig.Fire, 40.f, FVector(0, 0, 0));
	UARPGElementalVolumeComponent* Rock =
		SpawnVolume(Scope.World, Earth, 25.f, FVector(100, 0, 0));

	Rig.Reactions->Resolve(Fireball, Rock);

	// Energy is still exchanged and both sides weaken -- they just produce
	// nothing visible. Two spells meeting should always cost each other
	// something, whether or not anyone authored what they make together.
	TestEqual(TEXT("The smaller is spent"), Rock->GetEnergy(), 0.f);
	TestEqual(TEXT("The larger is reduced"), Fireball->GetEnergy(), 15.f);
	TestNull(TEXT("But nothing is produced"), Rig.Reactions->GetLastProduct());

	Rig.Reactions->bNeutraliseWithoutProduct = false;

	UARPGElementalVolumeComponent* SecondFire =
		SpawnVolume(Scope.World, Rig.Fire, 40.f, FVector(0, 0, 0));
	UARPGElementalVolumeComponent* SecondRock =
		SpawnVolume(Scope.World, Earth, 25.f, FVector(100, 0, 0));

	Rig.Reactions->Resolve(SecondFire, SecondRock);
	TestEqual(TEXT("Turned off, they pass through untouched"), SecondFire->GetEnergy(), 40.f);
	TestEqual(TEXT("Both of them"), SecondRock->GetEnergy(), 25.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGReactionAmplifyTest,
	"ARPG.World.Reaction.AmplificationFeedsTheSurvivor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGReactionAmplifyTest::RunTest(const FString& Parameters)
{
	using namespace ARPGReactionTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UARPGMagicElement* Air = MakeElement(Rig.Table, TAG_Element_Air);

	// fire + air -> FIRE. The result being one of the reactants IS the whole
	// declaration; no separate amplification concept exists.
	UARPGMagicCombinationEntry* Fanned =
		MakeEntry(Rig.Table, TAG_Element_Fire, TAG_Element_Air, Rig.Fire);
	Fanned->AmplificationEfficiency = 0.5f;
	Rig.Table->Entries.Add(Fanned);

	UARPGElementalVolumeComponent* Fireball =
		SpawnVolume(Scope.World, Rig.Fire, 40.f, FVector(0, 0, 0));
	UARPGElementalVolumeComponent* Gust =
		SpawnVolume(Scope.World, Air, 30.f, FVector(100, 0, 0));

	Rig.Reactions->Resolve(Fireball, Gust);

	// The survivor eats the WHOLE of the loser, at the row's efficiency. There
	// is no partial exchange and no product burst, because the product IS the
	// survivor.
	TestEqual(TEXT("The gust is eaten entirely"), Gust->GetEnergy(), 0.f);
	TestEqual(TEXT("And the fireball grows by the efficiency"), Fireball->GetEnergy(), 55.f);
	TestTrue(TEXT("So it reports above its original energy"),
		Fireball->GetEnergy() > Fireball->GetInitialEnergy());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGReactionRatesTest,
	"ARPG.World.Reaction.ConsumptionRatesMakeItLopsided",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGReactionRatesTest::RunTest(const FString& Parameters)
{
	using namespace ARPGReactionTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	// Water quenches fire far faster than fire boils off water. The same numbers
	// drive rain against a grass fire in the spread system -- one authored row,
	// two solvers.
	UARPGMagicCombinationEntry* Entry = Rig.Table->Entries[0];
	Entry->ConsumptionRates.Add(TAG_Element_Fire, 2.f);
	Entry->ConsumptionRates.Add(TAG_Element_Water, 0.5f);

	UARPGElementalVolumeComponent* Fireball =
		SpawnVolume(Scope.World, Rig.Fire, 100.f, FVector(0, 0, 0));
	UARPGElementalVolumeComponent* Jet =
		SpawnVolume(Scope.World, Rig.Water, 40.f, FVector(100, 0, 0));

	Rig.Reactions->Resolve(Fireball, Jet);

	// Reacted is 40 either way; the rates decide what each side pays for it.
	TestEqual(TEXT("Fire pays double"), Fireball->GetEnergy(), 20.f);
	TestEqual(TEXT("Water pays half, so the jet punches through"), Jet->GetEnergy(), 20.f);

	return true;
}

// ---------------------------------------------------------------------------
// Conduction
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGConductionChainTest,
	"ARPG.World.Conduction.ChargeFollowsTouchingMedia",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGConductionChainTest::RunTest(const FString& Parameters)
{
	using namespace ARPGReactionTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UARPGConductionSubsystem* Conduction = Scope.World->GetSubsystem<UARPGConductionSubsystem>();
	if (!Conduction)
	{
		AddError(TEXT("Setup: no conduction subsystem in the test world."));
		return false;
	}

	UARPGMagicElement* Lightning = MakeElement(Rig.Table, TAG_Element_Lightning);

	UARPGMagicCombinationEntry* Conducts =
		MakeEntry(Rig.Table, TAG_Element_Lightning, TAG_Element_Water, Lightning);
	Conducts->Mode = EARPGReactionMode::Conduct;
	Rig.Table->Entries.Add(Conducts);

	Conduction->CombinationTable = Rig.Table;
	Conduction->DistanceLoss = 0.f; // isolate the per-hop conductivity term
	Conduction->MinEnergy = 1.f;

	// Three puddles in a row, each overlapping the next. Nothing authors the
	// connection -- touching IS the connection.
	UARPGElementalVolumeComponent* P1 =
		SpawnVolume(Scope.World, Rig.Water, 0.f, FVector(0, 0, 0), 100.f);
	UARPGElementalVolumeComponent* P2 =
		SpawnVolume(Scope.World, Rig.Water, 0.f, FVector(150, 0, 0), 100.f);
	UARPGElementalVolumeComponent* P3 =
		SpawnVolume(Scope.World, Rig.Water, 0.f, FVector(300, 0, 0), 100.f);

	for (UARPGElementalVolumeComponent* Puddle : { P1, P2, P3 })
	{
		Puddle->Conductivity = 1.f;
	}

	ResolveOverlaps({ P1, P2, P3 });

	const int32 Reached = Conduction->Conduct(Lightning, P1, 100.f, nullptr);

	TestEqual(TEXT("The charge runs the whole chain"), Reached, 3);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGConductionBreakTest,
	"ARPG.World.Conduction.NonConductorBreaksTheChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGConductionBreakTest::RunTest(const FString& Parameters)
{
	using namespace ARPGReactionTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UARPGConductionSubsystem* Conduction = Scope.World->GetSubsystem<UARPGConductionSubsystem>();
	UARPGMagicElement* Lightning = MakeElement(Rig.Table, TAG_Element_Lightning);

	UARPGMagicCombinationEntry* Conducts =
		MakeEntry(Rig.Table, TAG_Element_Lightning, TAG_Element_Water, Lightning);
	Conducts->Mode = EARPGReactionMode::Conduct;
	Rig.Table->Entries.Add(Conducts);

	Conduction->CombinationTable = Rig.Table;
	Conduction->DistanceLoss = 0.f;

	UARPGElementalVolumeComponent* P1 =
		SpawnVolume(Scope.World, Rig.Water, 0.f, FVector(0, 0, 0), 100.f);
	UARPGElementalVolumeComponent* P2 =
		SpawnVolume(Scope.World, Rig.Water, 0.f, FVector(150, 0, 0), 100.f);

	P1->Conductivity = 1.f;

	// The middle puddle carries nothing -- dry ground, or a pane missing from a
	// run of glass. The same filter that drops every projectile out of a flood.
	P2->Conductivity = 0.f;

	ResolveOverlaps({ P1, P2 });

	const int32 Reached = Conduction->Conduct(Lightning, P1, 100.f, nullptr);

	TestEqual(TEXT("A non-conductor stops the charge"), Reached, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGConductionFalloffTest,
	"ARPG.World.Conduction.EnergyFadesAlongTheChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGConductionFalloffTest::RunTest(const FString& Parameters)
{
	using namespace ARPGReactionTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UARPGConductionSubsystem* Conduction = Scope.World->GetSubsystem<UARPGConductionSubsystem>();
	UARPGMagicElement* Lightning = MakeElement(Rig.Table, TAG_Element_Lightning);

	UARPGMagicCombinationEntry* Conducts =
		MakeEntry(Rig.Table, TAG_Element_Lightning, TAG_Element_Water, Lightning);
	Conducts->Mode = EARPGReactionMode::Conduct;
	Rig.Table->Entries.Add(Conducts);

	Conduction->CombinationTable = Rig.Table;
	Conduction->DistanceLoss = 0.f;

	// Poor conductors: each hop keeps only a fraction, so the charge dies out
	// partway down a chain rather than reaching everything equally.
	UARPGElementalVolumeComponent* P1 =
		SpawnVolume(Scope.World, Rig.Water, 0.f, FVector(0, 0, 0), 100.f);
	UARPGElementalVolumeComponent* P2 =
		SpawnVolume(Scope.World, Rig.Water, 0.f, FVector(150, 0, 0), 100.f);
	UARPGElementalVolumeComponent* P3 =
		SpawnVolume(Scope.World, Rig.Water, 0.f, FVector(300, 0, 0), 100.f);

	for (UARPGElementalVolumeComponent* Puddle : { P1, P2, P3 })
	{
		Puddle->Conductivity = 0.2f;
	}

	Conduction->MinEnergy = 1.f;
	ResolveOverlaps({ P1, P2, P3 });

	// 100 -> 20 entering, -> 4 at the second, -> 0.8 at the third, which is
	// below MinEnergy and therefore never arrives.
	const int32 Reached = Conduction->Conduct(Lightning, P1, 100.f, nullptr);

	TestEqual(TEXT("The charge dies out before the last puddle"), Reached, 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGConductionUnauthoredTest,
	"ARPG.World.Conduction.WithoutARowNothingConducts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGConductionUnauthoredTest::RunTest(const FString& Parameters)
{
	using namespace ARPGReactionTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UARPGConductionSubsystem* Conduction = Scope.World->GetSubsystem<UARPGConductionSubsystem>();
	UARPGMagicElement* Lightning = MakeElement(Rig.Table, TAG_Element_Lightning);

	// No Conduct row at all. Water carries lightning; stone does not -- and
	// which is which is authored, not coded.
	Conduction->CombinationTable = Rig.Table;

	UARPGElementalVolumeComponent* P1 =
		SpawnVolume(Scope.World, Rig.Water, 0.f, FVector(0, 0, 0), 100.f);
	UARPGElementalVolumeComponent* P2 =
		SpawnVolume(Scope.World, Rig.Water, 0.f, FVector(150, 0, 0), 100.f);

	P1->Conductivity = 1.f;
	P2->Conductivity = 1.f;

	ResolveOverlaps({ P1, P2 });

	// The entry medium is still struck -- the charge was delivered into it --
	// but it goes no further without a row saying it travels.
	const int32 Reached = Conduction->Conduct(Lightning, P1, 100.f, nullptr);
	TestEqual(TEXT("It spreads to nothing"), Reached, 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS
