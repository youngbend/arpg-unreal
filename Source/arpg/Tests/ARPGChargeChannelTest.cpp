// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ARPGAttackDefinition.h"
#include "ARPGCombatDummy.h"
#include "ARPGComboComponent.h"
#include "ARPGVitalSet.h"
#include "ARPGWeaponAttackTree.h"
#include "AbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

/**
 * Charge and channel, at the combo-component level.
 *
 * WHAT THIS DELIBERATELY DOES NOT COVER. The montage half -- stretching the
 * Windup section over ChargeTime, jumping to Active on release, self-linking
 * Active so a channel loops -- lives in UARPGGameplayAbility_MeleeAttack and
 * needs authored montage sections to mean anything. Everything below is the
 * state machine that DECIDES those things, which is where the Godot version's
 * bugs actually lived and which is verifiable with no content at all.
 *
 * The two halves meet at three events: Event.Attack.Begin, .ChargeRelease and
 * .ChannelStop. Those are asserted here as state transitions rather than as
 * dispatched events, because no ability is granted in this fixture.
 */
namespace ARPGChargeChannelTestUtils
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

	struct FRig
	{
		AARPGCombatDummy* Actor = nullptr;
		UARPGComboComponent* Combo = nullptr;
		UARPGWeaponAttackTree* Tree = nullptr;
		UARPGAttackDefinition* Attack = nullptr;

		float Stamina() const
		{
			return Actor->GetAbilitySystemComponent()->GetNumericAttribute(
				UARPGVitalSet::GetStaminaAttribute());
		}

		void SetStamina(float Value) const
		{
			Actor->GetAbilitySystemComponent()->SetNumericAttributeBase(
				UARPGVitalSet::GetStaminaAttribute(), Value);
		}
	};

	/** One root attack on Light, configured by the caller before use. */
	FRig BuildRig(UWorld* World)
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

		Rig.Attack = NewObject<UARPGAttackDefinition>(Rig.Tree);
		Rig.Attack->AttackId = TEXT("test_attack");
		Rig.Attack->StaminaCost = 0.f;

		UARPGComboAttackNode* Root = NewObject<UARPGComboAttackNode>(Rig.Tree);
		Root->Attack = Rig.Attack;
		Rig.Tree->RootLight = Root;

		Rig.Combo = NewObject<UARPGComboComponent>(Rig.Actor);
		Rig.Combo->RegisterComponent();
		Rig.Combo->AttackTree = Rig.Tree;

		return Rig;
	}
}

// ---------------------------------------------------------------------------
// Charge
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGChargeReleaseTest,
	"ARPG.Combat.Charge.ReleaseScalesWithHoldTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGChargeReleaseTest::RunTest(const FString& Parameters)
{
	using namespace ARPGChargeChannelTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Attack->bChargeable = true;
	Rig.Attack->ChargeTime = 1.f;
	Rig.Attack->ChargeMotionMin = 0.5f;
	Rig.Attack->ChargeMotionMax = 2.5f;
	Rig.Attack->StaminaCost = 30.f;

	Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
	TestTrue(TEXT("Press begins a charge"), Rig.Combo->IsCharging());

	// Deferred cost is the whole reason charge does not route through
	// StartAttack: an interrupted charge has to be free.
	TestEqual(TEXT("Charging has not spent stamina yet"), Rig.Stamina(), 100.f);

	Tick(Rig.Combo, 0.5f);
	Rig.Combo->ReceiveInputReleased(EARPGAttackInput::Light);

	TestFalse(TEXT("Release ends the charge"), Rig.Combo->IsCharging());
	TestTrue(TEXT("Release starts the swing"), Rig.Combo->IsAttacking());
	TestEqual(TEXT("Charge fraction reflects the hold"), Rig.Combo->GetChargeFraction(), 0.5f, 0.05f);
	TestEqual(TEXT("Stamina is spent on release"), Rig.Stamina(), 70.f);

	// The fraction only matters because it scales the swing.
	TestEqual(TEXT("Half charge lands halfway up the motion range"),
		Rig.Attack->GetChargedMotionValue(Rig.Combo->GetChargeFraction()), 1.5f, 0.06f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGChargeAutoReleaseTest,
	"ARPG.Combat.Charge.AutoReleasesWhenFull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGChargeAutoReleaseTest::RunTest(const FString& Parameters)
{
	using namespace ARPGChargeChannelTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Attack->bChargeable = true;
	Rig.Attack->ChargeTime = 0.5f;
	Rig.Attack->ChargeMotionMin = 0.5f;
	Rig.Attack->ChargeMotionMax = 2.f;

	Rig.Combo->ReceiveInput(EARPGAttackInput::Light);

	// Holding past full does not keep charging: the swing goes off by itself,
	// which is what stops a player parking at full charge indefinitely.
	Tick(Rig.Combo, 0.8f);

	TestFalse(TEXT("Full charge released itself"), Rig.Combo->IsCharging());
	TestTrue(TEXT("The swing started"), Rig.Combo->IsAttacking());
	TestEqual(TEXT("Auto-release is a full charge"), Rig.Combo->GetChargeFraction(), 1.f, 0.02f);
	TestEqual(TEXT("Full charge hits the top of the range"),
		Rig.Attack->GetChargedMotionValue(Rig.Combo->GetChargeFraction()), 2.f, 0.02f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGChargeInterruptedTest,
	"ARPG.Combat.Charge.InterruptionCostsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGChargeInterruptedTest::RunTest(const FString& Parameters)
{
	using namespace ARPGChargeChannelTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Attack->bChargeable = true;
	Rig.Attack->ChargeTime = 1.f;
	Rig.Attack->StaminaCost = 30.f;

	Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
	Tick(Rig.Combo, 0.4f);

	// Flinched or dodged out of the charge.
	Rig.Combo->CancelAttack();

	TestFalse(TEXT("Interruption ends the charge"), Rig.Combo->IsCharging());
	TestFalse(TEXT("Nothing is swinging"), Rig.Combo->IsAttacking());
	TestEqual(TEXT("An interrupted charge is free"), Rig.Stamina(), 100.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGChargeStarvedTest,
	"ARPG.Combat.Charge.ReleaseWithoutStaminaFizzles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGChargeStarvedTest::RunTest(const FString& Parameters)
{
	using namespace ARPGChargeChannelTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Attack->bChargeable = true;
	Rig.Attack->ChargeTime = 1.f;
	Rig.Attack->StaminaCost = 40.f;

	Rig.SetStamina(10.f);

	Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
	TestTrue(TEXT("Setup: charging begins regardless of stamina"), Rig.Combo->IsCharging());

	Tick(Rig.Combo, 0.5f);
	Rig.Combo->ReceiveInputReleased(EARPGAttackInput::Light);

	// Because the cost is deferred to release, the check has to happen there too
	// -- otherwise a charge begun with full stamina and released after paying for
	// something else would swing for free.
	TestFalse(TEXT("No swing without the stamina for it"), Rig.Combo->IsAttacking());
	TestEqual(TEXT("A fizzled release spends nothing"), Rig.Stamina(), 10.f);

	return true;
}

// ---------------------------------------------------------------------------
// Channel
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGChannelLoopTest,
	"ARPG.Combat.Channel.LoopsAndDrains",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGChannelLoopTest::RunTest(const FString& Parameters)
{
	using namespace ARPGChargeChannelTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Attack->bChannel = true;
	Rig.Attack->StaminaCost = 10.f;
	Rig.Attack->ChannelStaminaPerSecond = 20.f;

	Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
	TestTrue(TEXT("Press begins the channel"), Rig.Combo->IsChanneling());
	TestFalse(TEXT("But not the loop -- the wind-up plays first"), Rig.Combo->IsChannelLooping());
	TestEqual(TEXT("The opening cost is paid at the press"), Rig.Stamina(), 90.f);

	// A channel must NOT drain during its wind-up: the player is not getting a
	// hitbox yet, so charging them for it would make a long wind-up a tax.
	Tick(Rig.Combo, 0.5f);
	TestEqual(TEXT("The wind-up does not drain"), Rig.Stamina(), 90.f);

	TestTrue(TEXT("Held through the wind-up, so the loop starts"),
		Rig.Combo->NotifyChannelWindupFinished());
	TestTrue(TEXT("Now looping"), Rig.Combo->IsChannelLooping());

	Tick(Rig.Combo, 1.f);
	TestEqual(TEXT("The loop drains at the authored rate"), Rig.Stamina(), 70.f, 1.f);

	Rig.Combo->ReceiveInputReleased(EARPGAttackInput::Light);
	TestFalse(TEXT("Release stops the loop"), Rig.Combo->IsChannelLooping());
	TestTrue(TEXT("But the channel is still closing out its recovery"), Rig.Combo->IsChanneling());

	const float AfterRelease = Rig.Stamina();
	Tick(Rig.Combo, 0.5f);
	TestEqual(TEXT("The recovery does not drain either"), Rig.Stamina(), AfterRelease);

	Rig.Combo->NotifyChannelEnded();
	TestFalse(TEXT("Recovery finished, channel closed"), Rig.Combo->IsChanneling());
	TestFalse(TEXT("And nothing is attacking"), Rig.Combo->IsAttacking());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGChannelTapTest,
	"ARPG.Combat.Channel.TapDoesNotLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGChannelTapTest::RunTest(const FString& Parameters)
{
	using namespace ARPGChargeChannelTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Attack->bChannel = true;
	Rig.Attack->ChannelStaminaPerSecond = 20.f;

	Rig.Combo->ReceiveInput(EARPGAttackInput::Light);

	// Let go during the wind-up. The release cannot end a loop that has not
	// started, so it has to be remembered until the wind-up asks.
	Rig.Combo->ReceiveInputReleased(EARPGAttackInput::Light);
	Tick(Rig.Combo, 0.3f);

	TestFalse(TEXT("A tap never enters the loop"), Rig.Combo->NotifyChannelWindupFinished());
	TestFalse(TEXT("And is not looping"), Rig.Combo->IsChannelLooping());
	TestEqual(TEXT("So nothing drained"), Rig.Stamina(), 100.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGChannelExhaustionTest,
	"ARPG.Combat.Channel.ExhaustionEndsTheLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGChannelExhaustionTest::RunTest(const FString& Parameters)
{
	using namespace ARPGChargeChannelTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Attack->bChannel = true;
	Rig.Attack->ChannelStaminaPerSecond = 50.f;
	Rig.SetStamina(20.f);

	Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
	TestTrue(TEXT("Setup: the loop starts"), Rig.Combo->NotifyChannelWindupFinished());

	// Held down throughout -- only stamina stops this.
	Tick(Rig.Combo, 2.f);

	TestFalse(TEXT("Running dry ends the loop"), Rig.Combo->IsChannelLooping());
	TestTrue(TEXT("Stamina never goes negative"), Rig.Stamina() >= 0.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGChannelInterruptedTest,
	"ARPG.Combat.Channel.InterruptionClearsState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGChannelInterruptedTest::RunTest(const FString& Parameters)
{
	using namespace ARPGChargeChannelTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	Rig.Attack->bChannel = true;
	Rig.Attack->ChannelStaminaPerSecond = 10.f;

	Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
	Rig.Combo->NotifyChannelWindupFinished();
	Tick(Rig.Combo, 0.3f);

	// Flinched out of the drill. The ability that would have called
	// NotifyChannelEnded is the thing being cancelled, so if CancelAttack does
	// not clear the channel flags nothing ever will -- and every later press is
	// swallowed by a channel that is not running.
	Rig.Combo->CancelAttack();

	TestFalse(TEXT("Interruption clears channelling"), Rig.Combo->IsChanneling());
	TestFalse(TEXT("And the loop"), Rig.Combo->IsChannelLooping());
	TestFalse(TEXT("And the attack"), Rig.Combo->IsAttacking());

	const float AfterCancel = Rig.Stamina();
	Tick(Rig.Combo, 1.f);
	TestEqual(TEXT("A cancelled channel stops draining"), Rig.Stamina(), AfterCancel);

	// The real proof it is not wedged: a fresh press still works.
	Rig.Attack->bChannel = false;
	Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
	TestTrue(TEXT("The character can attack again"), Rig.Combo->IsAttacking());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
