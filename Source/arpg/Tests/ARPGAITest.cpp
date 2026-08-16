// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ARPGAILibrary.h"
#include "ARPGCombatDummy.h"
#include "ARPGGameplayTags.h"
#include "ARPGNoiseComponent.h"
#include "ARPGPerceptionComponent.h"
#include "AbilitySystemComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

/**
 * The AI layer.
 *
 * WHAT IS COVERED. Perception's three channels and their retention rules, and
 * the parry's contact prediction. Both are pure enough to pin exactly, and both
 * are where the interesting decisions live.
 *
 * WHAT IS NOT. Tree TOPOLOGY -- which state runs when -- is authored in a
 * StateTree asset and belongs to content rather than code; and the tasks and
 * conditions need a running UStateTreeAIComponent with an AI controller and a
 * compiled tree, which is a level's worth of fixture for logic that is a handful
 * of lines over components already tested elsewhere.
 */
namespace ARPGAITestUtils
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

	/** A faction-tagged dummy with a pawn-channel collider, so scans find it. */
	AARPGCombatDummy* SpawnCharacter(UWorld* World, FGameplayTag Faction, const FVector& Location)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AARPGCombatDummy* Actor = World->SpawnActorDeferred<AARPGCombatDummy>(
			AARPGCombatDummy::StaticClass(), FTransform(Location));
		Actor->FactionTag = Faction;
		Actor->FinishSpawning(FTransform(Location));

		if (UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(Actor->GetRootComponent()))
		{
			Root->SetMobility(EComponentMobility::Movable);
			Root->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Root->SetCollisionObjectType(ECC_Pawn);
			Root->SetCollisionResponseToAllChannels(ECR_Overlap);
			Root->SetWorldLocation(Location);
		}

		// Explicitly, rather than relying on the dummy's own BeginPlay: an
		// unresolved faction makes CanDamage return true for everything, which
		// would let a faction test pass for the wrong reason.
		if (UAbilitySystemComponent* ASC = Actor->GetAbilitySystemComponent())
		{
			ASC->InitAbilityActorInfo(Actor, Actor);
			ASC->AddLooseGameplayTag(Faction, 1, EGameplayTagReplicationState::TagOnly);
		}

		return Actor;
	}

	UARPGPerceptionComponent* AddPerception(AActor* Actor)
	{
		UARPGPerceptionComponent* Perception = NewObject<UARPGPerceptionComponent>(Actor);
		Perception->RegisterComponent();
		return Perception;
	}
}

// ---------------------------------------------------------------------------
// Perception
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPerceptionAcquireTest,
	"ARPG.AI.Perception.AcquiresTheNearestHostile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPerceptionAcquireTest::RunTest(const FString& Parameters)
{
	using namespace ARPGAITestUtils;
	FTestWorld Scope;

	AARPGCombatDummy* Guard = SpawnCharacter(Scope.World, TAG_Faction_Enemy, FVector::ZeroVector);
	AARPGCombatDummy* Near = SpawnCharacter(Scope.World, TAG_Faction_Player, FVector(300, 0, 0));
	AARPGCombatDummy* Far = SpawnCharacter(Scope.World, TAG_Faction_Player, FVector(900, 0, 0));

	UARPGPerceptionComponent* Perception = AddPerception(Guard);
	Perception->DetectionRange = 1200.f;

	Perception->Scan(0.1f);

	TestNotNull(TEXT("A hostile in range is acquired"), Perception->GetTarget());
	TestEqual(TEXT("And it is the nearest one"), Perception->GetTarget(), Cast<AActor>(Near));
	TestTrue(TEXT("Acquiring alerts the NPC"), Perception->IsAlerted());

	// An ally is not a target, through the same faction filter the hitbox uses --
	// so what an NPC will fight and what it can damage never disagree.
	AARPGCombatDummy* Ally = SpawnCharacter(Scope.World, TAG_Faction_Enemy, FVector(100, 0, 0));
	Perception->SetTarget(nullptr);
	Perception->Scan(0.1f);

	TestNotEqual(TEXT("An ally is never targeted, however close"),
		Perception->GetTarget(), Cast<AActor>(Ally));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPerceptionRangeTest,
	"ARPG.AI.Perception.NothingOutsideDetectionRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPerceptionRangeTest::RunTest(const FString& Parameters)
{
	using namespace ARPGAITestUtils;
	FTestWorld Scope;

	AARPGCombatDummy* Guard = SpawnCharacter(Scope.World, TAG_Faction_Enemy, FVector::ZeroVector);
	SpawnCharacter(Scope.World, TAG_Faction_Player, FVector(2000, 0, 0));

	UARPGPerceptionComponent* Perception = AddPerception(Guard);
	Perception->DetectionRange = 500.f;

	Perception->Scan(0.1f);
	TestNull(TEXT("Beyond detection range, nothing is seen"), Perception->GetTarget());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPerceptionFOVTest,
	"ARPG.AI.Perception.ConeGatesAcquisitionButNotRetention",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPerceptionFOVTest::RunTest(const FString& Parameters)
{
	using namespace ARPGAITestUtils;
	FTestWorld Scope;

	AARPGCombatDummy* Guard = SpawnCharacter(Scope.World, TAG_Faction_Enemy, FVector::ZeroVector);

	// Directly behind the guard, which faces +X by default.
	AARPGCombatDummy* Sneak = SpawnCharacter(Scope.World, TAG_Faction_Player, FVector(-300, 0, 0));

	UARPGPerceptionComponent* Perception = AddPerception(Guard);
	Perception->DetectionRange = 1200.f;
	Perception->FOVAngle = 90.f;

	TestFalse(TEXT("Behind the cone, a candidate cannot be acquired"),
		Perception->CanSee(Sneak, /*bApplyFOV=*/true));

	// THE RULE THIS EXISTS FOR. The cone gates ACQUISITION only: an NPC mid-fight
	// does not lose its target because it turned its back for a moment. Retention
	// asks the same question with the cone switched off.
	TestTrue(TEXT("But the same candidate is still perceived for retention"),
		Perception->CanSee(Sneak, /*bApplyFOV=*/false));

	Perception->Scan(0.1f);
	TestNull(TEXT("So a scan does not acquire them"), Perception->GetTarget());

	// Once locked on, turning away does not drop them.
	Perception->SetTarget(Sneak);
	Perception->Scan(0.1f);
	TestEqual(TEXT("An engaged target survives the NPC turning its back"),
		Perception->GetTarget(), Cast<AActor>(Sneak));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPerceptionHearingTest,
	"ARPG.AI.Perception.HearingIgnoresRangeAndCone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPerceptionHearingTest::RunTest(const FString& Parameters)
{
	using namespace ARPGAITestUtils;
	FTestWorld Scope;

	AARPGCombatDummy* Guard = SpawnCharacter(Scope.World, TAG_Faction_Enemy, FVector::ZeroVector);
	AARPGCombatDummy* Sprinter = SpawnCharacter(Scope.World, TAG_Faction_Player, FVector(-2000, 0, 0));

	UARPGPerceptionComponent* Perception = AddPerception(Guard);
	Perception->DetectionRange = 500.f; // far too short to see them
	Perception->FOVAngle = 90.f;        // and they are behind

	TestFalse(TEXT("Setup: unseeable"), Perception->CanSee(Sprinter, /*bApplyFOV=*/true));
	TestFalse(TEXT("Setup: and silent so far"), Perception->CanHear(Sprinter));

	UARPGNoiseComponent* Noise = NewObject<UARPGNoiseComponent>(Sprinter);
	Noise->IdleRadius = 3000.f; // loud even standing still, for a deterministic test
	Noise->RegisterComponent();

	// Hearing is its own channel: a loud enough sound carries through a wall,
	// from behind, and well past the visual detection range.
	TestTrue(TEXT("A loud enough candidate is heard regardless"),
		Perception->CanHear(Sprinter));

	Perception->Scan(0.1f);
	TestEqual(TEXT("And that alone acquires them"),
		Perception->GetTarget(), Cast<AActor>(Sprinter));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPerceptionMemoryTest,
	"ARPG.AI.Perception.MemoryHoldsALostTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPerceptionMemoryTest::RunTest(const FString& Parameters)
{
	using namespace ARPGAITestUtils;
	FTestWorld Scope;

	AARPGCombatDummy* Guard = SpawnCharacter(Scope.World, TAG_Faction_Enemy, FVector::ZeroVector);
	AARPGCombatDummy* Runner = SpawnCharacter(Scope.World, TAG_Faction_Player, FVector(300, 0, 0));

	UARPGPerceptionComponent* Perception = AddPerception(Guard);
	Perception->DetectionRange = 500.f;
	Perception->MemoryDuration = 1.f;

	Perception->Scan(0.1f);
	TestEqual(TEXT("Setup: acquired"), Perception->GetTarget(), Cast<AActor>(Runner));

	const FVector LastSeen = Runner->GetActorLocation();

	// Out of range entirely.
	Runner->SetActorLocation(FVector(5000, 0, 0));

	Perception->Scan(0.5f);
	TestEqual(TEXT("Within the memory window the target is held"),
		Perception->GetTarget(), Cast<AActor>(Runner));
	TestEqual(TEXT("At where it was last actually perceived, not where it now is"),
		Perception->GetLastKnownLocation(), LastSeen);

	Perception->Scan(0.6f);
	TestNull(TEXT("Past the window it is dropped"), Perception->GetTarget());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPerceptionInvestigateTest,
	"ARPG.AI.Perception.UnseenAttackerTriggersInvestigation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPerceptionInvestigateTest::RunTest(const FString& Parameters)
{
	using namespace ARPGAITestUtils;
	FTestWorld Scope;

	AARPGCombatDummy* Guard = SpawnCharacter(Scope.World, TAG_Faction_Enemy, FVector::ZeroVector);
	AARPGCombatDummy* Sniper = SpawnCharacter(Scope.World, TAG_Faction_Player, FVector(5000, 0, 0));

	UARPGPerceptionComponent* Perception = AddPerception(Guard);
	Perception->DetectionRange = 500.f;
	Perception->DamageReaction = EARPGDamageReaction::Investigate;

	Perception->NotifyDamagedBy(Sniper, Sniper->GetActorLocation());

	// The point of the investigate reaction: being shot from the dark does NOT
	// reveal the shooter. The NPC goes to look, and only genuinely engages if its
	// own senses then find something -- which is what makes a stealth approach
	// survivable after a first hit.
	TestNull(TEXT("An unseen attacker grants no target"), Perception->GetTarget());
	TestFalse(TEXT("Nor alerts the NPC"), Perception->IsAlerted());

	// It records WHERE, though, which is what a tree branch walks the NPC to.
	TestEqual(TEXT("But records where the blow came from"),
		Perception->GetLastKnownLocation(), Sniper->GetActorLocation());

	// The investigate location is its own channel, read by the StateTree
	// evaluator and bound from there. It used to be written straight into a
	// blackboard key, which meant nothing could assert on it without staging a
	// whole AI controller -- so this rule went untested until the migration.
	TestTrue(TEXT("An investigation is flagged as having somewhere to go"),
		Perception->HasInvestigateLocation());
	TestEqual(TEXT("And that somewhere is where the blow came from"),
		Perception->GetInvestigateLocation(), Sniper->GetActorLocation());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPerceptionInvestigateOutlivesTargetTest,
	"ARPG.AI.Perception.InvestigationOutlivesTheTargetItFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPerceptionInvestigateOutlivesTargetTest::RunTest(const FString& Parameters)
{
	using namespace ARPGAITestUtils;
	FTestWorld Scope;

	AARPGCombatDummy* Guard = SpawnCharacter(Scope.World, TAG_Faction_Enemy, FVector::ZeroVector);
	AARPGCombatDummy* Sniper = SpawnCharacter(Scope.World, TAG_Faction_Player, FVector(5000, 0, 0));

	UARPGPerceptionComponent* Perception = AddPerception(Guard);
	Perception->DetectionRange = 500.f;
	Perception->DamageReaction = EARPGDamageReaction::Investigate;

	const FVector Origin = Sniper->GetActorLocation();
	Perception->NotifyDamagedBy(Sniper, Origin);

	// Acquiring and then losing a target must not erase the place that started
	// it: an NPC that investigates, finds someone, fights and loses them should
	// go back to where the first blow came from rather than to nowhere.
	Perception->SetTarget(Sniper);
	Perception->SetTarget(nullptr);

	TestTrue(TEXT("The investigation survives a target coming and going"),
		Perception->HasInvestigateLocation());
	TestEqual(TEXT("Still pointing at the original blow"),
		Perception->GetInvestigateLocation(), Origin);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPerceptionAggroTest,
	"ARPG.AI.Perception.AggroReactionLocksOnImmediately",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPerceptionAggroTest::RunTest(const FString& Parameters)
{
	using namespace ARPGAITestUtils;
	FTestWorld Scope;

	AARPGCombatDummy* Guard = SpawnCharacter(Scope.World, TAG_Faction_Enemy, FVector::ZeroVector);
	AARPGCombatDummy* Sniper = SpawnCharacter(Scope.World, TAG_Faction_Player, FVector(5000, 0, 0));
	AARPGCombatDummy* Other = SpawnCharacter(Scope.World, TAG_Faction_Player, FVector(6000, 0, 0));

	UARPGPerceptionComponent* Perception = AddPerception(Guard);
	Perception->DetectionRange = 500.f;
	Perception->DamageReaction = EARPGDamageReaction::Aggro;

	Perception->NotifyDamagedBy(Sniper, Sniper->GetActorLocation());

	// Bypasses range, cone and line of sight entirely: getting hit reveals the
	// attacker whether or not the NPC had spotted them.
	TestEqual(TEXT("Aggro locks on from any distance"),
		Perception->GetTarget(), Cast<AActor>(Sniper));

	// And having committed, a hit from someone ELSE does not pull it off --
	// otherwise a crowd fight becomes every NPC swapping targets each graze.
	Perception->NotifyDamagedBy(Other, Other->GetActorLocation());
	TestEqual(TEXT("A third party's hit does not steal aggro"),
		Perception->GetTarget(), Cast<AActor>(Sniper));

	return true;
}

// ---------------------------------------------------------------------------
// Reactive parry
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGParryPredictionTest,
	"ARPG.AI.Parry.PredictsContactFromClosingKinematics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGParryPredictionTest::RunTest(const FString& Parameters)
{
	const float ContactRadius = 60.f;

	// Constant speed: 260cm of gap at 200cm/s is one second.
	TestEqual(TEXT("Constant closing speed is a linear solve"),
		UARPGAILibrary::PredictTimeToContact(320.f, 200.f, 0.f, ContactRadius),
		1.3f, 0.01f);

	// Already inside contact radius: now, not a negative time.
	TestEqual(TEXT("Already touching predicts immediate contact"),
		UARPGAILibrary::PredictTimeToContact(40.f, 200.f, 0.f, ContactRadius), 0.f);

	// Not closing at all: no prediction rather than an infinite or negative one.
	TestTrue(TEXT("A stationary hitbox yields no prediction"),
		UARPGAILibrary::PredictTimeToContact(320.f, 0.f, 0.f, ContactRadius) < 0.f);

	// Accelerating: 260cm at 100cm/s plus 200cm/s^2 solves to ~1.06s, sooner
	// than the 2.6s a constant-speed read would give. This is the whole reason
	// the acceleration term exists -- a real swing ramps hard in its last frames.
	const float Accelerating =
		UARPGAILibrary::PredictTimeToContact(320.f, 100.f, 200.f, ContactRadius);
	TestTrue(TEXT("Acceleration brings contact forward"), Accelerating > 0.f && Accelerating < 1.3f);

	// Decelerating hard enough never to arrive: no prediction, so the NPC does
	// not raise its guard at a swing that is petering out.
	TestTrue(TEXT("An approach that never arrives yields no prediction"),
		UARPGAILibrary::PredictTimeToContact(320.f, 50.f, -100.f, ContactRadius) < 0.f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
