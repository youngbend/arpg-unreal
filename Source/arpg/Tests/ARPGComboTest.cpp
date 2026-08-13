// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ARPGAttackDefinition.h"
#include "ARPGCombatDummy.h"
#include "ARPGComboComponent.h"
#include "ARPGGameplayTags.h"
#include "ARPGTestSwingAugment.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGComboTest,
	"ARPG.Combat.Combo",
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

/**
 * A specific-attack imbue taking over a beat.
 *
 * The behaviour under test is the trade the mechanic is built on: the element's
 * move pre-empts whatever the tree had queued up, and ENDS THE CHAIN by doing
 * it. Nothing here knows about magic -- the combo component asks an interface,
 * and a stand-in answers.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGComboAugmentOverrideTest,
	"ARPG.Combat.ImbuedAttackOverridesTheChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGComboAugmentOverrideTest::RunTest(const FString& Parameters)
{
	using namespace ARPGComboTestUtils;

	FTestWorld TestWorld;
	UWorld* World = TestWorld.World;

	FRig Rig = BuildRig(World);
	UAbilitySystemComponent* ASC = Rig.Actor->GetAbilitySystemComponent();

	UARPGAttackDefinition* Slash = MakeAttack(TEXT("MagnetismFloatingSlash"));

	FGameplayAbilitySpec Spec(UARPGTestSwingAugment::StaticClass(), 1);
	const FGameplayAbilitySpecHandle Handle = ASC->GiveAbility(Spec);

	// Nothing readied yet: the tree answers for itself.
	Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
	TestEqual(TEXT("with no augment the tree resolves normally"),
		Rig.Combo->GetCurrentNode(), Rig.L1);
	Rig.Combo->NotifyAttackFinished();

	// Ready the coating. Only light is overridden, which is the partial-authoring
	// case: an element with one move must not break the other two buttons.
	TestTrue(TEXT("the stand-in augment activates"), ASC->TryActivateAbility(Handle));

	UARPGTestSwingAugment* Augment = nullptr;
	if (const FGameplayAbilitySpec* Found = ASC->FindAbilitySpecFromHandle(Handle))
	{
		Augment = Cast<UARPGTestSwingAugment>(Found->GetPrimaryInstance());
	}

	if (!Augment)
	{
		AddError(TEXT("no augment instance -- the rest of this case cannot run"));
		return false;
	}

	Augment->OverrideLight = Slash;

	// Mid-chain: the tree would have gone L1 -> L2. The element takes the beat.
	Rig.Combo->ReceiveInput(EARPGAttackInput::Light);

	UARPGComboAttackNode* OverrideNode = Rig.Combo->GetCurrentNode();
	TestNotEqual(TEXT("the element's move replaces the follow-up"), OverrideNode, Rig.L2);
	TestEqual(TEXT("and it is the element's own attack"),
		OverrideNode ? OverrideNode->Attack.Get() : nullptr, Slash);

	// THE COST. A leaf is how this component says the chain stops here, and it is
	// what makes the next press start over rather than continuing to L3.
	TestTrue(TEXT("the override beat is a leaf, so the chain ends on it"),
		OverrideNode && OverrideNode->IsLeaf());

	// An unoverridden button still resolves through the tree while the same
	// coating is readied.
	Rig.Combo->NotifyAttackFinished();
	Rig.Combo->ReceiveInput(EARPGAttackInput::Heavy);
	TestEqual(TEXT("a button the element has no move for falls back to the tree"),
		Rig.Combo->GetCurrentNode(), Rig.Tree->GetRoot(EARPGAttackInput::Heavy));

	// And with the override cleared, light returns to the chain from root.
	Augment->OverrideLight = nullptr;
	Rig.Combo->NotifyAttackFinished();
	Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
	TestEqual(TEXT("once spent, the tree has the beat back"),
		Rig.Combo->GetCurrentNode(), Rig.L1);

	return true;
}

/**
 * Chain depth: what an attack that ends the chain is paid for ending it.
 *
 * Without this the cheapest moment to throw a chain-ender is from neutral, where
 * there is no chain to lose -- so using it late is strictly worse and the
 * "should I cash in or push for one more beat" decision does not exist. The
 * counter has to measure beats ALREADY PLAYED, and an override must not count
 * itself, or a move would be paid for the chain it just ended.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGComboChainDepthTest,
	"ARPG.Combat.ChainDepthPaysForTheChainSpent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGComboChainDepthTest::RunTest(const FString& Parameters)
{
	using namespace ARPGComboTestUtils;

	FTestWorld TestWorld;
	UWorld* World = TestWorld.World;

	// --- The counter ----------------------------------------------------------
	{
		FRig Rig = BuildRig(World);

		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		TestEqual(TEXT("a swing from neutral has nothing behind it"),
			Rig.Combo->GetChainDepth(), 0);

		Rig.Combo->NotifyAttackFinished();
		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		TestEqual(TEXT("the second beat follows one"), Rig.Combo->GetChainDepth(), 1);

		Rig.Combo->NotifyAttackFinished();
		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		TestEqual(TEXT("and the third follows two"), Rig.Combo->GetChainDepth(), 2);

		// Back to root: the chain is gone and so is what it was worth.
		Rig.Combo->NotifyAttackFinished();
		Rig.Combo->ResetCombo();
		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		TestEqual(TEXT("a reset chain is worth nothing again"),
			Rig.Combo->GetChainDepth(), 0);
	}

	// --- An override reads the chain it interrupts, and does not extend it -----
	{
		FRig Rig = BuildRig(World);
		UAbilitySystemComponent* ASC = Rig.Actor->GetAbilitySystemComponent();

		FGameplayAbilitySpec Spec(UARPGTestSwingAugment::StaticClass(), 1);
		const FGameplayAbilitySpecHandle Handle = ASC->GiveAbility(Spec);
		ASC->TryActivateAbility(Handle);

		UARPGTestSwingAugment* Augment = nullptr;
		if (const FGameplayAbilitySpec* Found = ASC->FindAbilitySpecFromHandle(Handle))
		{
			Augment = Cast<UARPGTestSwingAugment>(Found->GetPrimaryInstance());
		}

		if (!Augment)
		{
			AddError(TEXT("no augment instance -- the rest of this case cannot run"));
			return false;
		}

		Augment->OverrideHeavy = MakeAttack(TEXT("MagnetismSpin"));

		// Two beats of chain, then cash it in.
		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		Rig.Combo->NotifyAttackFinished();
		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		Rig.Combo->NotifyAttackFinished();

		Rig.Combo->ReceiveInput(EARPGAttackInput::Heavy);
		TestEqual(TEXT("the element's move is paid for the chain it spends"),
			Rig.Combo->GetChainDepth(), 2);
	}

	// --- The scaling itself ---------------------------------------------------
	{
		UARPGAttackDefinition* Toss = MakeAttack(TEXT("MagnetismToss"));
		Toss->MotionValue = 1.6f;
		Toss->ChainDepthBonus = 0.35f;
		Toss->MaxChainDepth = 3;

		TestEqual(TEXT("from neutral it is worth exactly its base"),
			Toss->GetChainScaledMotionValue(Toss->MotionValue, 0), 1.6f);
		TestEqual(TEXT("three beats in it is worth 2.05x"),
			Toss->GetChainScaledMotionValue(Toss->MotionValue, 3), 1.6f * 2.05f);

		// Capped, so a long chain cannot be farmed into an unbounded hit.
		TestEqual(TEXT("past the cap it stops growing"),
			Toss->GetChainScaledMotionValue(Toss->MotionValue, 9),
			Toss->GetChainScaledMotionValue(Toss->MotionValue, 3));

		// Off by default, so nothing already authored changes.
		UARPGAttackDefinition* Plain = MakeAttack(TEXT("L1"));
		Plain->MotionValue = 1.f;
		TestEqual(TEXT("an attack with no bonus is untouched by depth"),
			Plain->GetChainScaledMotionValue(1.f, 5), 1.f);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
