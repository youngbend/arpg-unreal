// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "ARPGTestFixtures.h"

#if WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS

#include "ARPGCombatDummy.h"
#include "ARPGGameplayTags.h"
#include "ARPGGeometryProbe.h"
#include "ARPGHitStopComponent.h"
#include "ARPGParryComponent.h"
#include "ARPGPlayerActionComponent.h"
#include "ARPGReactionDefinitions.h"
#include "ARPGStatusEffectComponent.h"
#include "ARPGStatusEffects.h"
#include "ARPGStatusVfxSubsystem.h"
#include "ARPGTestVfxActor.h"
#include "ARPGWeaponAttackTree.h"
#include "ARPGWeaponComponent.h"
#include "ARPGWeaponDefinition.h"
#include "AbilitySystemComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"

/**
 * The feedback layer: fitted status visuals, hit-stop, and per-weapon reactions.
 *
 * All of it is presentation, which makes it easy to under-test on the grounds
 * that nothing gameplay reads it. What is actually worth pinning is the small
 * set of rules that make the presentation AFFORDABLE and correct: measurement
 * that does not include the effect it is measuring for, a budget that genuinely
 * bounds what is alive, and a freeze that cannot accumulate into a lockup.
 */
namespace ARPGFeedbackTestUtils
{
	using ARPGTest::FTestWorld;

	template <typename TComponent>
	TComponent* AddComponent(AActor* Owner, const TCHAR* Name)
	{
		TComponent* Component = NewObject<TComponent>(Owner, Name);
		Component->RegisterComponent();
		return Component;
	}

	/** The engine's unit cube: 100 units across, so half-extent 50. */
	UStaticMesh* LoadCube()
	{
		return LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	}

	/**
	 * An actor made of a small mesh at the origin and a tall collider above it.
	 *
	 * Deliberately shaped so the union and the mesh-only answer DIFFER, which is
	 * the whole distinction the probe exists to draw.
	 */
	AActor* SpawnMixedGeometry(UWorld* World, UStaticMesh* Cube)
	{
		AActor* Actor = World->SpawnActor<AActor>();

		UStaticMeshComponent* Mesh = AddComponent<UStaticMeshComponent>(Actor, TEXT("Mesh"));
		Actor->SetRootComponent(Mesh);
		Mesh->SetStaticMesh(Cube);
		Mesh->SetWorldLocation(FVector::ZeroVector);

		// Reaches from z=100 to z=500 -- well clear of the cube's own top at 50.
		// Parented BEFORE registering: SetupAttachment only initialises the link
		// for a future attach, and on an already-registered component it warns
		// and does nothing.
		UBoxComponent* Box = NewObject<UBoxComponent>(Actor, TEXT("Collider"));
		Box->SetupAttachment(Mesh);
		Box->RegisterComponent();
		Box->SetBoxExtent(FVector(30.f, 30.f, 200.f));
		Box->SetRelativeLocation(FVector(0.f, 0.f, 300.f));

		return Actor;
	}

	/**
	 * A stand-in for an authored VFX actor: a bare scene root and nothing else.
	 *
	 * The root matters. A root-less actor cannot be attached or placed, so the
	 * probe refuses it -- and a fixture without one would be testing that
	 * refusal rather than the fitting. No geometry, though: anything with bounds
	 * of its own would inflate the target it is being measured against.
	 */
	AActor* SpawnVfxStandIn(UWorld* World)
	{
		AActor* Actor = World->SpawnActor<AActor>();
		USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
		Actor->SetRootComponent(Root);
		Root->RegisterComponent();
		return Actor;
	}

	/** A target whose only geometry is one predictable box. */
	AActor* SpawnBoxTarget(UWorld* World, const FVector& Extent, const FVector& Offset)
	{
		AActor* Actor = World->SpawnActor<AActor>();
		UBoxComponent* Box = AddComponent<UBoxComponent>(Actor, TEXT("Box"));
		Actor->SetRootComponent(Box);
		Box->SetBoxExtent(Extent);
		Box->SetWorldLocation(Offset);
		return Actor;
	}

	/** A status effect built at runtime, so a test can name its own visual. */
	UGameplayEffect* MakeVisibleStatus(FGameplayTag StatusTag, int32 Priority,
		TSubclassOf<AActor> VfxClass, EARPGStatusVfxFit Fit = EARPGStatusVfxFit::Bounds)
	{
		// A plain UGameplayEffect, not UARPGStatusGameplayEffect: that subclass is
		// abstract, and nothing here needs its stack-extension behaviour. What
		// the subsystem actually looks for is the presentation component below.
		UGameplayEffect* Effect = NewObject<UGameplayEffect>(GetTransientPackage());
		Effect->DurationPolicy = EGameplayEffectDurationType::Infinite;

		UARPGStatusEffectComponent& Presentation =
			Effect->AddComponent<UARPGStatusEffectComponent>();
		Presentation.StatusTag = StatusTag;
		Presentation.VfxPriority = Priority;
		Presentation.VfxFit = Fit;
		Presentation.VfxActorClass = TSoftClassPtr<AActor>(VfxClass);

		return Effect;
	}

	void Tick(UActorComponent* Component, float Seconds, float Step = 0.05f)
	{
		for (float Elapsed = 0.f; Elapsed < Seconds; Elapsed += Step)
		{
			Component->TickComponent(Step, LEVELTICK_All, nullptr);
		}
	}
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGProbeBoundsTest,
	"ARPG.VFX.Probe.UnionsMeshesAndCollidersButCanIsolateMeshes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGProbeBoundsTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFeedbackTestUtils;

	FTestWorld Fixture;

	UStaticMesh* Cube = LoadCube();
	TestNotNull(TEXT("The engine's unit cube is available"), Cube);
	if (!Cube)
	{
		return false;
	}

	AActor* Actor = SpawnMixedGeometry(Fixture.World, Cube);

	FBox Union(ForceInit);
	TestTrue(TEXT("An actor made of geometry measures"),
		ARPGGeometryProbe::MeasureLocalBounds(Actor, Union));

	// The union reaches the top of the COLLIDER. Preferring meshes and falling
	// back to colliders would stop at the cube, and everything asking how far
	// this thing reaches would get an answer 450 units short.
	TestEqual(TEXT("The union reaches the collider's top"),
		static_cast<float>(Union.Max.Z), 500.f, 1.f);
	TestEqual(TEXT("And down to the cube's underside"),
		static_cast<float>(Union.Min.Z), -50.f, 1.f);

	FBox VisualOnly(ForceInit);
	TestTrue(TEXT("Mesh-only measures too"),
		ARPGGeometryProbe::MeasureLocalBounds(Actor, VisualOnly, /*bVisualOnly=*/true));

	// The surface question. A collider that deliberately stands above the thing
	// it describes must not drag a surface effect up into that headroom.
	TestEqual(TEXT("Mesh-only stops at the mesh"),
		static_cast<float>(VisualOnly.Max.Z), 50.f, 1.f);

	// Footprint is the HORIZONTAL extent: a tall thin thing must not claim a
	// footprint it does not have.
	TestEqual(TEXT("Footprint ignores height"),
		static_cast<float>(ARPGGeometryProbe::BoundsFootprint(Union)), 100.f, 1.f);

	// An actor with nothing to measure says so rather than reporting a point at
	// the origin, which would silently collapse every effect fitted to it.
	AActor* Empty = Fixture.World->SpawnActor<AActor>();
	FBox Unmeasured(ForceInit);
	TestFalse(TEXT("An actor made of nothing does not measure"),
		ARPGGeometryProbe::MeasureLocalBounds(Empty, Unmeasured));

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGProbeFitTest,
	"ARPG.VFX.Fit.EachModePlacesAndScalesFromMeasuredBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGProbeFitTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFeedbackTestUtils;

	FTestWorld Fixture;

	// 200 x 200 x 600 centred on the actor: half-extents 100/100/300.
	AActor* Target = SpawnBoxTarget(Fixture.World, FVector(100.f, 100.f, 300.f), FVector::ZeroVector);

	auto FitOne = [&](EARPGStatusVfxFit Mode, float Scale) -> AActor*
	{
		AActor* Vfx = SpawnVfxStandIn(Fixture.World);
		ARPGGeometryProbe::FitVfxToTarget(Vfx, Target, Mode, Scale, /*Stacks=*/1);
		return Vfx;
	};

	{
		AActor* Vfx = FitOne(EARPGStatusVfxFit::Bounds, 1.f);
		TestEqual(TEXT("Bounds centres on the middle"),
			static_cast<float>(Vfx->GetRootComponent()->GetRelativeLocation().Z), 0.f, 0.5f);
		// The largest dimension, so the effect fills the whole thing.
		TestEqual(TEXT("Bounds scales to the largest dimension"),
			static_cast<float>(Vfx->GetRootComponent()->GetRelativeScale3D().X), 600.f, 1.f);
	}

	{
		AActor* Vfx = FitOne(EARPGStatusVfxFit::Base, 1.f);
		TestEqual(TEXT("Base sits on the floor of the bounds"),
			static_cast<float>(Vfx->GetRootComponent()->GetRelativeLocation().Z), -300.f, 0.5f);
		// The FOOTPRINT, not the height: a puddle under a tall creature should
		// be as wide as the creature, not as wide as it is tall.
		TestEqual(TEXT("And scales to the footprint"),
			static_cast<float>(Vfx->GetRootComponent()->GetRelativeScale3D().X), 200.f, 1.f);
	}

	{
		AActor* Vfx = FitOne(EARPGStatusVfxFit::Top, 1.f);
		TestEqual(TEXT("Top hovers at the ceiling of the bounds"),
			static_cast<float>(Vfx->GetRootComponent()->GetRelativeLocation().Z), 300.f, 0.5f);
	}

	{
		AActor* Vfx = FitOne(EARPGStatusVfxFit::None, 2.f);
		TestEqual(TEXT("None sits at the origin"),
			static_cast<float>(Vfx->GetRootComponent()->GetRelativeLocation().Z), 0.f, 0.5f);
		// Unscaled by the bounds, but the effect's own multiplier still applies
		// -- that knob is authored intent, not a fitting artefact.
		TestEqual(TEXT("And takes only the authored multiplier"),
			static_cast<float>(Vfx->GetRootComponent()->GetRelativeScale3D().X), 2.f, 0.01f);
	}

	{
		// The effect's multiplier compounds with the fit, so an effect that
		// should read larger than the thing carrying it can say so.
		AActor* Vfx = FitOne(EARPGStatusVfxFit::Base, 1.5f);
		TestEqual(TEXT("The multiplier scales the fitted size"),
			static_cast<float>(Vfx->GetRootComponent()->GetRelativeScale3D().X), 300.f, 1.f);
	}

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGProbeFittableTest,
	"ARPG.VFX.Fit.AnImplementerIsHandedBoundsAndNotScaled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGProbeFittableTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFeedbackTestUtils;

	FTestWorld Fixture;
	AActor* Target = SpawnBoxTarget(Fixture.World, FVector(100.f, 100.f, 300.f), FVector::ZeroVector);

	AARPGTestVfxActor* Vfx = Fixture.World->SpawnActor<AARPGTestVfxActor>();
	ARPGGeometryProbe::FitVfxToTarget(Vfx, Target, EARPGStatusVfxFit::Bounds, 3.f, /*Stacks=*/4);

	TestTrue(TEXT("An implementer is handed the bounds"), Vfx->bFitCalled);
	TestEqual(TEXT("Measured from the target, in its local space"),
		static_cast<float>(Vfx->FittedBounds.Max.Z), 300.f, 1.f);
	TestEqual(TEXT("Along with the stack count"), Vfx->LastStacks, 4);

	// NOT SCALED. It has the bounds and is expected to act on them; scaling it
	// as well would double-apply what it has already been told.
	TestEqual(TEXT("And is emphatically not scaled on top"),
		static_cast<float>(Vfx->GetRootComponent()->GetRelativeScale3D().X), 1.f, 0.01f);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGVfxBudgetTest,
	"ARPG.VFX.Budget.PriorityThenDistanceDecidesWhoIsVisible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGVfxBudgetTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFeedbackTestUtils;

	FTestWorld Fixture;

	UARPGStatusVfxSubsystem* Vfx = Fixture.World->GetSubsystem<UARPGStatusVfxSubsystem>();
	TestNotNull(TEXT("The world has a status VFX subsystem"), Vfx);
	if (!Vfx)
	{
		return false;
	}

	// No player controller in a bare test world, so the anchor is the origin --
	// which makes distance from the origin the distance that sorts.
	Vfx->CullDistance = 100000.f;

	UGameplayEffect* Faint = MakeVisibleStatus(TAG_Status_Weakened, /*Priority=*/0,
		AARPGTestVfxActor::StaticClass());
	UGameplayEffect* Loud = MakeVisibleStatus(TAG_Status_Burning, /*Priority=*/10,
		AARPGTestVfxActor::StaticClass());

	// Near carries the LOW priority effect and Far the high one, so priority and
	// distance disagree -- otherwise the test cannot tell which one is sorting.
	AARPGCombatDummy* Near = Fixture.World->SpawnActor<AARPGCombatDummy>(
		FVector(100.f, 0.f, 0.f), FRotator::ZeroRotator);
	AARPGCombatDummy* Far = Fixture.World->SpawnActor<AARPGCombatDummy>(
		FVector(5000.f, 0.f, 0.f), FRotator::ZeroRotator);

	UAbilitySystemComponent* NearASC = Near->GetAbilitySystemComponent();
	UAbilitySystemComponent* FarASC = Far->GetAbilitySystemComponent();

	NearASC->ApplyGameplayEffectToSelf(Faint, 1.f, NearASC->MakeEffectContext());
	FarASC->ApplyGameplayEffectToSelf(Loud, 1.f, FarASC->MakeEffectContext());

	Vfx->MaxInstances = 2;
	Vfx->Reconcile();

	FARPGStatusVfxStats Stats = Vfx->GetStats();

	// Registration is the ability system's own doing, on component initialise --
	// nothing here asked for it. That is the property worth protecting: the
	// visual layer discovers what is afflicted at zero authoring cost.
	TestEqual(TEXT("Both ability systems registered themselves"), Stats.Registered, 2);
	TestEqual(TEXT("Both afflictions want a visual"), Stats.Requests, 2);
	TestEqual(TEXT("And the budget affords both"), Stats.Live, 2);

	// One seat left. The DISTANT high-priority effect should keep it: a
	// character on fire matters more than a faint shimmer, wherever they are.
	Vfx->MaxInstances = 1;
	Vfx->Reconcile();

	Stats = Vfx->GetStats();
	TestEqual(TEXT("Both still want one"), Stats.Requests, 2);
	TestEqual(TEXT("The budget bounds what is ALIVE, not just what is new"),
		Stats.Live, 1);

	int32 BurningVisible = 0;
	for (TActorIterator<AARPGTestVfxActor> It(Fixture.World); It; ++It)
	{
		// Finish rather than Destroy, so the loser is still around to be asked.
		if (!It->bFinishCalled && It->GetAttachParentActor() == Far)
		{
			++BurningVisible;
		}
	}
	TestEqual(TEXT("And the seat went to priority, not to proximity"), BurningVisible, 1);

	// Room again: the loser gets its visual back rather than being forgotten.
	// Wanting and having are separate for exactly this reason.
	Vfx->MaxInstances = 4;
	Vfx->Reconcile();
	TestEqual(TEXT("Raising the budget restores what it evicted"),
		Vfx->GetStats().Live, 2);

	// The effect ends and the request goes with it.
	FarASC->RemoveActiveEffects(FGameplayEffectQuery());
	Vfx->Reconcile();

	Stats = Vfx->GetStats();
	TestEqual(TEXT("An ended affliction stops wanting a visual"), Stats.Requests, 1);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGHitStopTest,
	"ARPG.Combat.HitStop.TakesTheLongestRemainingRatherThanTheSum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGHitStopTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFeedbackTestUtils;

	FTestWorld Fixture;
	AARPGCombatDummy* Dummy = Fixture.World->SpawnActor<AARPGCombatDummy>();

	// NOT "Mesh": the dummy already has a static mesh component under that name,
	// and reusing it is a fatal object replacement rather than a shadowed name.
	USkeletalMeshComponent* Mesh =
		AddComponent<USkeletalMeshComponent>(Dummy, TEXT("SkeletalMesh"));
	Mesh->GlobalAnimRateScale = 1.f;

	UARPGHitStopComponent* HitStop = AddComponent<UARPGHitStopComponent>(Dummy, TEXT("HitStop"));
	HitStop->MaxDuration = 1.f;

	HitStop->ApplyHitStop(0.3f);
	TestTrue(TEXT("A hit freezes the animation"), HitStop->IsFrozen());
	TestEqual(TEXT("The mesh stops advancing"), Mesh->GlobalAnimRateScale, 0.f);

	Tick(HitStop, 0.1f);
	TestEqual(TEXT("And counts down"), HitStop->GetRemaining(), 0.2f, 0.02f);

	// THE RULE THIS TEST EXISTS FOR. A shorter follow-up hit lands: the freeze
	// must not extend. Summing would turn a busy exchange into a lockup, and it
	// would get worse exactly when the fight got busiest.
	HitStop->ApplyHitStop(0.15f);
	TestEqual(TEXT("A shorter follow-up does not extend the freeze"),
		HitStop->GetRemaining(), 0.2f, 0.02f);

	// A longer one does, because that hit genuinely asks for more.
	HitStop->ApplyHitStop(0.5f);
	TestEqual(TEXT("A longer one does"), HitStop->GetRemaining(), 0.5f, 0.02f);

	// And the re-entrant freezes must not have snapshotted the already-zeroed
	// rate -- that is the bug that leaves a character stopped forever.
	Tick(HitStop, 0.6f);
	TestFalse(TEXT("The freeze ends"), HitStop->IsFrozen());
	TestEqual(TEXT("And the animation rate comes back, not the frozen zero"),
		Mesh->GlobalAnimRateScale, 1.f);

	// Clamped: a slipped decimal point in authored data is a character frozen
	// for ten seconds with no error to say why.
	HitStop->ApplyHitStop(99.f);
	TestEqual(TEXT("An absurd duration is clamped"),
		HitStop->GetRemaining(), 1.f, 0.02f);

	Tick(HitStop, 1.2f);

	// Never on a corpse: the death animation has to play out.
	Dummy->GetAbilitySystemComponent()->AddLooseGameplayTag(TAG_State_Dead);
	HitStop->ApplyHitStop(0.3f);
	TestFalse(TEXT("A corpse is never frozen"), HitStop->IsFrozen());
	TestEqual(TEXT("Its death animation keeps playing"), Mesh->GlobalAnimRateScale, 1.f);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGBlockDefinitionTest,
	"ARPG.Combat.Block.TimingsComeFromTheEquippedWeapon",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGBlockDefinitionTest::RunTest(const FString& Parameters)
{
	using namespace ARPGFeedbackTestUtils;

	FTestWorld Fixture;
	AARPGCombatDummy* Dummy = Fixture.World->SpawnActor<AARPGCombatDummy>();

	UARPGParryComponent* Parry = AddComponent<UARPGParryComponent>(Dummy, TEXT("Parry"));
	const float ComponentDefaultBlendIn = Parry->BlendInTime;

	UARPGWeaponComponent* Weapon = AddComponent<UARPGWeaponComponent>(Dummy, TEXT("Weapon"));
	UARPGPlayerActionComponent* Actions =
		AddComponent<UARPGPlayerActionComponent>(Dummy, TEXT("Actions"));

	// A greatshield: slow to raise, generous once up.
	UARPGBlockDefinition* Block = NewObject<UARPGBlockDefinition>(GetTransientPackage());
	Block->BlendInTime = 0.4f;
	Block->ParryWindow = 0.3f;
	Block->MovementSpeedFactor = 0.25f;

	UARPGWeaponAttackTree* Tree = NewObject<UARPGWeaponAttackTree>(GetTransientPackage());
	Tree->Block = Block;

	UARPGWeaponDefinition* Definition = NewObject<UARPGWeaponDefinition>(GetTransientPackage());
	Definition->AttackTree = Tree;

	Weapon->EquipWeapon(Definition);
	Weapon->SetDrawn(true);

	Actions->HandleParryPressed();

	TestTrue(TEXT("The guard goes up"), Parry->IsBlocking());
	TestEqual(TEXT("With the WEAPON's blend-in, not the component's"),
		Parry->BlendInTime, 0.4f);
	TestNotEqual(TEXT("Which really is a different number"),
		Parry->BlendInTime, ComponentDefaultBlendIn);
	TestEqual(TEXT("And the weapon's parry window"), Parry->ParryWindow, 0.3f);

	Actions->HandleParryReleased();
	TestFalse(TEXT("And comes back down"), Parry->IsBlocking());

	// A weapon with no block authored still defends, on the component's own
	// settings. An unfinished weapon should be playable, not defenceless.
	UARPGWeaponAttackTree* PlainTree = NewObject<UARPGWeaponAttackTree>(GetTransientPackage());
	UARPGWeaponDefinition* PlainWeapon = NewObject<UARPGWeaponDefinition>(GetTransientPackage());
	PlainWeapon->AttackTree = PlainTree;

	Parry->BlendInTime = ComponentDefaultBlendIn;
	Weapon->EquipWeapon(PlainWeapon);
	Weapon->SetDrawn(true);

	Actions->HandleParryPressed();
	TestTrue(TEXT("A weapon with no block definition still blocks"), Parry->IsBlocking());
	TestEqual(TEXT("On the component's own timings"),
		Parry->BlendInTime, ComponentDefaultBlendIn);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFlinchDefinitionTest,
	"ARPG.Combat.Flinch.ResolvesOneClipPerPoiseTier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFlinchDefinitionTest::RunTest(const FString& Parameters)
{
	UARPGFlinchDefinition* Flinch = NewObject<UARPGFlinchDefinition>(GetTransientPackage());

	// Nothing authored is the normal early state, and it must resolve to null
	// rather than to some other tier's clip -- a stance break playing a light
	// flinch would read as the reaction system being broken.
	TestNull(TEXT("Unauthored light flinch"),
		Flinch->GetMontageFor(EARPGPoiseResult::LightFlinch));
	TestNull(TEXT("Unauthored heavy flinch"),
		Flinch->GetMontageFor(EARPGPoiseResult::HeavyFlinch));
	TestNull(TEXT("Unauthored stance break"),
		Flinch->GetMontageFor(EARPGPoiseResult::StanceBreak));
	TestNull(TEXT("And None never has one"),
		Flinch->GetMontageFor(EARPGPoiseResult::None));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS
