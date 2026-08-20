// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ARPGAttackDefinition.h"
#include "ARPGCombatDummy.h"
#include "ARPGComboComponent.h"
#include "ARPGGameplayTags.h"
#include "ARPGVitalSet.h"
#include "ARPGWeaponAttackTree.h"
#include "AbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

/**
 * The combo state machine.
 *
 * Tested without any animation content: the component never touches montages --
 * it resolves tree position and raises events, and the ability does the rest.
 * That separation is what makes this half of phase 4 verifiable before a single
 * clip has been retargeted.
 */
namespace ARPGComboTestUtils
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

	/** The bare test world does not register component ticks -- drive them directly. */
	void Tick(UActorComponent* Component, float Seconds, float Step = 0.02f)
	{
		for (float Elapsed = 0.f; Elapsed < Seconds; Elapsed += Step)
		{
			Component->TickComponent(Step, LEVELTICK_All, nullptr);
		}
	}

	UARPGAttackDefinition* MakeAttack(const TCHAR* Id, float StaminaCost = 0.f)
	{
		UARPGAttackDefinition* Attack = NewObject<UARPGAttackDefinition>();
		Attack->AttackId = Id;
		Attack->StaminaCost = StaminaCost;
		return Attack;
	}

	UARPGComboAttackNode* MakeNode(UObject* Outer, UARPGAttackDefinition* Attack)
	{
		UARPGComboAttackNode* Node = NewObject<UARPGComboAttackNode>(Outer);
		Node->Attack = Attack;
		return Node;
	}

	struct FRig
	{
		AARPGCombatDummy* Actor = nullptr;
		UARPGComboComponent* Combo = nullptr;
		UARPGWeaponAttackTree* Tree = nullptr;

		/** L1 -> L2 -> L3, with L1 also branching to H2 on heavy. */
		UARPGComboAttackNode* L1 = nullptr;
		UARPGComboAttackNode* L2 = nullptr;
		UARPGComboAttackNode* L3 = nullptr;
		UARPGComboAttackNode* H2 = nullptr;

		float Stamina() const
		{
			return Actor->GetAbilitySystemComponent()->GetNumericAttribute(
				UARPGVitalSet::GetStaminaAttribute());
		}
	};

	FRig BuildRig(UWorld* World, float StaminaPerAttack = 0.f)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		FRig Rig;
		Rig.Actor = World->SpawnActor<AARPGCombatDummy>(
			AARPGCombatDummy::StaticClass(), FTransform::Identity, Params);

		UAbilitySystemComponent* ASC = Rig.Actor->GetAbilitySystemComponent();
		ASC->InitAbilityActorInfo(Rig.Actor, Rig.Actor);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxStaminaAttribute(), 100.f);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetStaminaAttribute(), 100.f);

		Rig.Tree = NewObject<UARPGWeaponAttackTree>();
		Rig.Tree->ResetTimeout = 1.5f;

		Rig.L3 = MakeNode(Rig.Tree, MakeAttack(TEXT("L3"), StaminaPerAttack));
		Rig.L2 = MakeNode(Rig.Tree, MakeAttack(TEXT("L2"), StaminaPerAttack));
		Rig.H2 = MakeNode(Rig.Tree, MakeAttack(TEXT("H2"), StaminaPerAttack));
		Rig.L1 = MakeNode(Rig.Tree, MakeAttack(TEXT("L1"), StaminaPerAttack));

		Rig.L2->FollowLight = Rig.L3;
		Rig.L1->FollowLight = Rig.L2;
		Rig.L1->FollowHeavy = Rig.H2;
		Rig.Tree->RootLight = Rig.L1;

		Rig.Combo = NewObject<UARPGComboComponent>(Rig.Actor);
		Rig.Combo->RegisterComponent();
		Rig.Combo->AttackTree = Rig.Tree;

		return Rig;
	}
}

// NAMED AS A LEAF UNDER "Combo", not as "ARPG.Combat.Combo" itself. The
// automation controller builds its tree by splitting these names on dots, so a
// test whose name is a strict prefix of another's becomes a PARENT node and
// stops being run -- silently, and still reported as a pass because it never
// reported anything. Adding NodeRegistry below is what would have done that to
// this test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGComboTest,
	"ARPG.Combat.Combo.StateMachine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGComboTest::RunTest(const FString& Parameters)
{
	using namespace ARPGComboTestUtils;

	FTestWorld TestWorld;
	UWorld* World = TestWorld.World;

	// --- Branching and root fallback -----------------------------------------
	{
		FRig Rig = BuildRig(World);

		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		TestEqual(TEXT("light from rest starts at the light root"),
			Rig.Combo->GetCurrentNode(), Rig.L1);
		TestTrue(TEXT("attacking"), Rig.Combo->IsAttacking());

		Rig.Combo->NotifyAttackFinished();
		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		TestEqual(TEXT("light again follows the chain"),
			Rig.Combo->GetCurrentNode(), Rig.L2);

		Rig.Combo->NotifyAttackFinished();
		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		TestEqual(TEXT("and again"), Rig.Combo->GetCurrentNode(), Rig.L3);

		// L3 has no light follow-up, so this falls back to root.
		Rig.Combo->NotifyAttackFinished();
		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		TestEqual(TEXT("a chain with no follow-up falls back to root"),
			Rig.Combo->GetCurrentNode(), Rig.L1);
	}

	// --- Branching on a different input ---------------------------------------
	{
		FRig Rig = BuildRig(World);

		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		Rig.Combo->NotifyAttackFinished();
		Rig.Combo->ReceiveInput(EARPGAttackInput::Heavy);
		TestEqual(TEXT("heavy branches off the light opener"),
			Rig.Combo->GetCurrentNode(), Rig.H2);
	}

	// --- Input buffering ------------------------------------------------------
	{
		FRig Rig = BuildRig(World);
		Rig.Combo->BufferWindow = 0.25f;

		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		const int32 SeqAfterFirst = Rig.Combo->GetAttackSequenceNumber();

		// Pressed mid-attack: buffered, not started.
		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		TestEqual(TEXT("a press mid-attack does not start a beat"),
			Rig.Combo->GetAttackSequenceNumber(), SeqAfterFirst);

		// The cancel window opens inside the buffer window, so it fires.
		Tick(Rig.Combo, 0.1f);
		Rig.Combo->NotifyAttackFinished();
		TestEqual(TEXT("a buffered press fires at the cancel window"),
			Rig.Combo->GetCurrentNode(), Rig.L2);
	}

	// --- Buffer expiry is measured from the PRESS -----------------------------
	// The property that stops a press early in a long windup committing the
	// player to their next action before they can see the swing resolve.
	{
		FRig Rig = BuildRig(World);
		Rig.Combo->BufferWindow = 0.25f;

		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		Rig.Combo->ReceiveInput(EARPGAttackInput::Light); // buffered

		Tick(Rig.Combo, 0.4f); // longer than the buffer window

		Rig.Combo->NotifyAttackFinished();
		TestEqual(TEXT("a stale buffered press is dropped, not honoured late"),
			Rig.Combo->GetCurrentNode(), Rig.L1);
		TestFalse(TEXT("and no new beat started"), Rig.Combo->IsAttacking());
	}

	// --- Continuous hold ------------------------------------------------------
	{
		FRig Rig = BuildRig(World);
		Rig.L1->Attack->bContinuousHold = true;

		Rig.Combo->ReceiveInput(EARPGAttackInput::Light); // held
		Rig.Combo->NotifyAttackFinished();

		TestEqual(TEXT("holding auto-continues the chain with no fresh press"),
			Rig.Combo->GetCurrentNode(), Rig.L2);
		TestTrue(TEXT("and it is a real new beat"), Rig.Combo->IsAttacking());
	}

	{
		FRig Rig = BuildRig(World);
		Rig.L1->Attack->bContinuousHold = true;

		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		Rig.Combo->ReceiveInputReleased(EARPGAttackInput::Light);
		Rig.Combo->NotifyAttackFinished();

		TestEqual(TEXT("released, the chain does not auto-continue"),
			Rig.Combo->GetCurrentNode(), Rig.L1);
		TestFalse(TEXT("and no beat is running"), Rig.Combo->IsAttacking());
	}

	// --- Finisher lockout -----------------------------------------------------
	// Blocks re-entry to root only. Mid-chain follow-ups stay responsive, and so
	// do dodge/parry interrupts, which never route through here.
	{
		FRig Rig = BuildRig(World);
		Rig.L3->Attack->FinisherLockout = 1.f;

		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		Rig.Combo->NotifyAttackFinished();
		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		Rig.Combo->NotifyAttackFinished();
		Rig.Combo->ReceiveInput(EARPGAttackInput::Light); // L3, the leaf
		TestEqual(TEXT("reached the finisher"), Rig.Combo->GetCurrentNode(), Rig.L3);

		Rig.Combo->NotifyAttackFinished();
		TestTrue(TEXT("finishing a leaf arms the lockout"),
			Rig.Combo->GetRootLockoutRemaining() > 0.f);

		const int32 SeqBefore = Rig.Combo->GetAttackSequenceNumber();
		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		TestEqual(TEXT("a new combo cannot start during the lockout"),
			Rig.Combo->GetAttackSequenceNumber(), SeqBefore);

		Tick(Rig.Combo, 1.1f);
		TestEqual(TEXT("lockout expires"), Rig.Combo->GetRootLockoutRemaining(), 0.f);
	}

	// A mid-chain node's lockout must NOT apply -- finisher-ness is tree
	// position, not a flag on the attack.
	{
		FRig Rig = BuildRig(World);
		Rig.L1->Attack->FinisherLockout = 1.f; // L1 is not a leaf

		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		Rig.Combo->NotifyAttackFinished();
		TestEqual(TEXT("a non-leaf node never arms the lockout"),
			Rig.Combo->GetRootLockoutRemaining(), 0.f);
	}

	// --- Stamina --------------------------------------------------------------
	{
		FRig Rig = BuildRig(World, /*StaminaPerAttack=*/30.f);

		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		TestEqual(TEXT("attacking costs stamina"), Rig.Stamina(), 70.f);

		Rig.Actor->GetAbilitySystemComponent()->SetNumericAttributeBase(
			UARPGVitalSet::GetStaminaAttribute(), 5.f);

		const int32 SeqBefore = Rig.Combo->GetAttackSequenceNumber();
		Rig.Combo->NotifyAttackFinished();
		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		TestEqual(TEXT("an unaffordable attack does not start"),
			Rig.Combo->GetAttackSequenceNumber(), SeqBefore);
		TestEqual(TEXT("and costs nothing"), Rig.Stamina(), 5.f);
	}

	// --- Attack sequence number -----------------------------------------------
	// IsAttacking() flips false then true inside one NotifyAttackFinished call
	// when chaining, so a poller never observes the gap. The counter is what
	// makes a chained beat detectable.
	{
		FRig Rig = BuildRig(World);
		Rig.L1->Attack->bContinuousHold = true;

		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		const int32 First = Rig.Combo->GetAttackSequenceNumber();

		Rig.Combo->NotifyAttackFinished(); // auto-chains within this call
		TestEqual(TEXT("a chained beat increments the sequence number"),
			Rig.Combo->GetAttackSequenceNumber(), First + 1);
		TestTrue(TEXT("and IsAttacking never appeared to go false"),
			Rig.Combo->IsAttacking());
	}

	// --- Reset timer ----------------------------------------------------------
	{
		FRig Rig = BuildRig(World);

		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		Rig.Combo->NotifyAttackFinished();
		TestEqual(TEXT("combo position is held after finishing"),
			Rig.Combo->GetCurrentNode(), Rig.L1);

		Tick(Rig.Combo, 1.6f); // past ResetTimeout
		TestNull(TEXT("the chain returns to root after the timeout"),
			Rig.Combo->GetCurrentNode());
	}

	// --- Attack lock ----------------------------------------------------------
	{
		FRig Rig = BuildRig(World);
		Rig.Combo->bAttackLocked = true;

		const int32 SeqBefore = Rig.Combo->GetAttackSequenceNumber();
		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		TestEqual(TEXT("no new attack while locked"),
			Rig.Combo->GetAttackSequenceNumber(), SeqBefore);
	}

	// --- Charge ---------------------------------------------------------------
	{
		FRig Rig = BuildRig(World);
		Rig.L1->Attack->bChargeable = true;
		Rig.L1->Attack->ChargeTime = 1.f;

		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		TestTrue(TEXT("a chargeable attack enters the charge phase"), Rig.Combo->IsCharging());
		TestFalse(TEXT("and has not swung yet"),
			Rig.Combo->GetAttackSequenceNumber() > 0);

		Tick(Rig.Combo, 0.3f);
		Rig.Combo->ReceiveInputReleased(EARPGAttackInput::Light);
		TestFalse(TEXT("releasing ends the charge"), Rig.Combo->IsCharging());
		TestTrue(TEXT("and swings"), Rig.Combo->GetAttackSequenceNumber() > 0);
	}

	{
		FRig Rig = BuildRig(World);
		Rig.L1->Attack->bChargeable = true;
		Rig.L1->Attack->ChargeTime = 0.2f;

		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		Tick(Rig.Combo, 0.4f); // held past full charge

		TestFalse(TEXT("a full charge auto-releases without a release press"),
			Rig.Combo->IsCharging());
		TestTrue(TEXT("and swings"), Rig.Combo->GetAttackSequenceNumber() > 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGComboNodeRegistryTest,
	"ARPG.Combat.Combo.NodeRegistry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * The registry a tree carries of its own nodes.
 *
 * Every tree in the project predates it -- they were built by wiring roots and
 * follow-ups directly -- so the walk that fills it in is what the graph editor
 * reads when it opens one of them. Two things can go wrong with a walk over a
 * structure that now permits sharing: counting a shared node twice, and never
 * finishing at all.
 */
bool FARPGComboNodeRegistryTest::RunTest(const FString& Parameters)
{
	using namespace ARPGComboTestUtils;

	UARPGWeaponAttackTree* Tree = NewObject<UARPGWeaponAttackTree>();

	UARPGComboAttackNode* Opener = MakeNode(Tree, MakeAttack(TEXT("opener")));
	UARPGComboAttackNode* Finisher = MakeNode(Tree, MakeAttack(TEXT("finisher")));

	// One node reached three ways, which is the sword tree's shape.
	Opener->FollowLight = Finisher;
	Opener->FollowHeavy = Finisher;
	Tree->RootLight = Opener;
	Tree->ParryLight = Finisher;

	Tree->RebuildNodeRegistry();

	TestEqual(TEXT("A shared node is registered once, not once per parent"),
		Tree->Nodes.Num(), 2);
	TestTrue(TEXT("The opener is registered"), Tree->Nodes.Contains(Opener));
	TestTrue(TEXT("The finisher is registered"), Tree->Nodes.Contains(Finisher));

	// A chain that returns to its own opener is legal: the combo component
	// resolves one step per press and never walks the structure. This walk does,
	// so it is the only thing a cycle could hang.
	Finisher->FollowLight = Opener;
	Tree->RebuildNodeRegistry();

	TestEqual(TEXT("A cycle terminates and registers each node once"), Tree->Nodes.Num(), 2);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
