// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ARPGCombatDummy.h"
#include "ARPGLocomotionComponent.h"
#include "ARPGModalInputComponent.h"
#include "ARPGModalInputTestListener.h"
#include "ARPGParryComponent.h"
#include "ARPGVitalSet.h"
#include "ARPGWeaponComponent.h"
#include "AbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

/**
 * The modal control scheme, the speed tiers, and auto-sheathe.
 *
 * What is worth protecting here is not that a button does a thing -- it is the
 * set of rules about what a button does NOT do. A modifier swallowing a press,
 * a charge outliving the trigger that started it, a potion refusing to queue:
 * every one of those is a deliberate refusal that looks like a bug from the
 * outside, and every one of them would be silently "fixed" by someone
 * simplifying the dispatch later.
 */
namespace ARPGInputTestUtils
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

	void Tick(UActorComponent* Component, float Seconds, float Step = 0.05f)
	{
		for (float Elapsed = 0.f; Elapsed < Seconds; Elapsed += Step)
		{
			Component->TickComponent(Step, LEVELTICK_All, nullptr);
		}
	}

	/** A bare owner. The modal component needs no pawn, controller or ASC. */
	AActor* SpawnHost(UWorld* World)
	{
		return World->SpawnActor<AActor>();
	}

	UARPGModalInputComponent* MakeInput(AActor* Owner)
	{
		UARPGModalInputComponent* Input =
			NewObject<UARPGModalInputComponent>(Owner, TEXT("ModalInput"));
		Input->RegisterComponent();
		return Input;
	}

	template <typename TComponent>
	TComponent* AddComponent(AActor* Owner, const TCHAR* Name)
	{
		TComponent* Component = NewObject<TComponent>(Owner, Name);
		Component->RegisterComponent();
		return Component;
	}
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGInputPlainFaceTest,
	"ARPG.Input.Modal.FaceButtonsMapToTheirPlainActionsWithNoModifier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGInputPlainFaceTest::RunTest(const FString& Parameters)
{
	using namespace ARPGInputTestUtils;

	FTestWorld Fixture;
	AActor* Host = SpawnHost(Fixture.World);
	UARPGModalInputComponent* Input = MakeInput(Host);

	UARPGModalInputTestListener* Listener = NewObject<UARPGModalInputTestListener>();
	Listener->Bind(Input);

	Input->PressFace(EARPGInputFace::North);
	Input->PressFace(EARPGInputFace::West);
	Input->PressFace(EARPGInputFace::South);
	Input->PressFace(EARPGInputFace::East);
	Input->PressSpecial();

	TestTrue(TEXT("Y is heavy"), Listener->Saw(TEXT("Heavy")));
	TestTrue(TEXT("X is light"), Listener->Saw(TEXT("Light")));
	TestTrue(TEXT("A is jump"), Listener->Saw(TEXT("Jump")));
	TestTrue(TEXT("B is dodge"), Listener->Saw(TEXT("Dodge")));
	TestTrue(TEXT("RB is the special attack"), Listener->Saw(TEXT("Special")));

	Listener->Clear();

	Input->ReleaseFace(EARPGInputFace::North);
	Input->ReleaseFace(EARPGInputFace::West);
	Input->ReleaseFace(EARPGInputFace::South);
	Input->ReleaseFace(EARPGInputFace::East);

	TestTrue(TEXT("Heavy has a release, because it charges"),
		Listener->Saw(TEXT("HeavyReleased")));
	TestTrue(TEXT("So does light"), Listener->Saw(TEXT("LightReleased")));

	// Jump and dodge are impulses. A release event for them would be an event
	// with nothing on the far end of it, and the first thing anyone would do
	// with one is bind something that fires twice per press.
	TestEqual(TEXT("Jump and dodge report only their presses"),
		Listener->Events.Num(), 2);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGInputMagicModifierTest,
	"ARPG.Input.Modal.MagicModifierRoutesFaceButtonsToElementSelect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGInputMagicModifierTest::RunTest(const FString& Parameters)
{
	using namespace ARPGInputTestUtils;

	FTestWorld Fixture;
	AActor* Host = SpawnHost(Fixture.World);
	UARPGModalInputComponent* Input = MakeInput(Host);

	UARPGModalInputTestListener* Listener = NewObject<UARPGModalInputTestListener>();
	Listener->Bind(Input);

	Input->SetMagicModifier(true);
	Input->PressFace(EARPGInputFace::West);

	// On the PRESS. Readying an element is a state the player acts from, so it
	// has to be true while their thumb is still down.
	TestTrue(TEXT("LT + X readies slot 1"), Listener->Saw(TEXT("MagicSelect:1")));
	TestFalse(TEXT("And emphatically does not swing"), Listener->Saw(TEXT("Light")));

	Listener->Clear();
	Input->ReleaseFace(EARPGInputFace::West);

	// The release is swallowed too. Letting it through would send a light-attack
	// release into the combo component off a button that never attacked.
	TestEqual(TEXT("Letting go under LT does nothing at all"),
		Listener->Events.Num(), 0);

	Listener->Clear();
	Input->PressSpecial();
	TestFalse(TEXT("LT + RB is swallowed like every other face button"),
		Listener->Saw(TEXT("Special")));

	Listener->Clear();
	Input->SetMagicModifier(false);
	Input->PressFace(EARPGInputFace::West);
	TestTrue(TEXT("And the plain meaning returns the moment LT comes up"),
		Listener->Saw(TEXT("Light")));

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGInputDPadTest,
	"ARPG.Input.Modal.DPadRoutesByModifierAndSwallowsMagicUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGInputDPadTest::RunTest(const FString& Parameters)
{
	using namespace ARPGInputTestUtils;

	FTestWorld Fixture;
	AActor* Host = SpawnHost(Fixture.World);
	UARPGModalInputComponent* Input = MakeInput(Host);

	UARPGModalInputTestListener* Listener = NewObject<UARPGModalInputTestListener>();
	Listener->Bind(Input);

	Input->PressDPad(EARPGInputDPad::Down);
	Input->PressDPad(EARPGInputDPad::Up);
	Input->PressDPad(EARPGInputDPad::Left);
	Input->PressDPad(EARPGInputDPad::Right);

	TestTrue(TEXT("Down sheathes"), Listener->Saw(TEXT("Sheathe")));
	TestTrue(TEXT("Up drinks"), Listener->Saw(TEXT("QuickSlotUse")));
	TestTrue(TEXT("Left and right cycle the bar"), Listener->Saw(TEXT("QuickSlotPrev")));
	TestTrue(TEXT("Left and right cycle the bar"), Listener->Saw(TEXT("QuickSlotNext")));

	Listener->Clear();
	Input->SetMagicModifier(true);

	Input->PressDPad(EARPGInputDPad::Down);
	Input->PressDPad(EARPGInputDPad::Left);
	Input->PressDPad(EARPGInputDPad::Right);

	TestTrue(TEXT("LT + Down discards what is readied"), Listener->Saw(TEXT("MagicDiscard")));
	TestTrue(TEXT("LT + Left pages back"), Listener->Saw(TEXT("MagicPagePrev")));
	TestTrue(TEXT("LT + Right pages on"), Listener->Saw(TEXT("MagicPageNext")));
	TestFalse(TEXT("None of which touch the quick-slot bar"),
		Listener->Saw(TEXT("QuickSlotPrev")));

	Listener->Clear();
	Input->PressDPad(EARPGInputDPad::Up);

	// THE RULE THIS TEST EXISTS FOR. Nothing is bound to LT + Up, and the press
	// is still consumed rather than falling through -- a player a frame late
	// letting go of LT must not drink a potion.
	TestEqual(TEXT("LT + Up is swallowed, not passed through to the quick slot"),
		Listener->Events.Num(), 0);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGInputChargeTest,
	"ARPG.Input.Modal.ChargeCompletesOnItsOwnFaceRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGInputChargeTest::RunTest(const FString& Parameters)
{
	using namespace ARPGInputTestUtils;

	FTestWorld Fixture;
	AActor* Host = SpawnHost(Fixture.World);
	UARPGModalInputComponent* Input = MakeInput(Host);
	Input->MaxChargeTime = 1.f;

	UARPGModalInputTestListener* Listener = NewObject<UARPGModalInputTestListener>();
	Listener->Bind(Input);

	Input->SetDischargeModifier(true);
	TestTrue(TEXT("RT's rising edge is announced on its own"),
		Listener->Saw(TEXT("DischargeModifier")));

	Input->PressFace(EARPGInputFace::West);
	TestTrue(TEXT("The wind-up is announced on the PRESS, so something can run it"),
		Listener->Saw(TEXT("ChargeStarted:1")));
	TestTrue(TEXT("And the component knows what is charging"), Input->IsCharging());
	TestFalse(TEXT("Nothing has fired yet"), Listener->Saw(TEXT("Discharge:1@0.50")));

	Tick(Input, 0.5f);

	TestEqual(TEXT("Half the maximum hold is half charge"),
		Input->GetChargeFraction(), 0.5f, 0.02f);

	Input->ReleaseFace(EARPGInputFace::West);

	TestTrue(TEXT("Letting go of X fires X's discharge at what it reached"),
		Listener->Saw(TEXT("Discharge:1@0.50")));
	TestFalse(TEXT("And is emphatically not a light attack release"),
		Listener->Saw(TEXT("LightReleased")));
	TestFalse(TEXT("The charge is over"), Input->IsCharging());

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGInputChargeOutlivesTriggerTest,
	"ARPG.Input.Modal.ReleasingTheTriggerDoesNotCancelACharge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGInputChargeOutlivesTriggerTest::RunTest(const FString& Parameters)
{
	using namespace ARPGInputTestUtils;

	FTestWorld Fixture;
	AActor* Host = SpawnHost(Fixture.World);
	UARPGModalInputComponent* Input = MakeInput(Host);
	Input->MaxChargeTime = 1.f;

	UARPGModalInputTestListener* Listener = NewObject<UARPGModalInputTestListener>();
	Listener->Bind(Input);

	Input->SetDischargeModifier(true);
	Input->PressFace(EARPGInputFace::North);

	// RT has done its job: it chose the mode. Requiring the player to keep it
	// down as well would make every charged discharge a two-finger hold.
	Input->SetDischargeModifier(false);
	Tick(Input, 1.5f);

	TestTrue(TEXT("The charge keeps running with the trigger up"), Input->IsCharging());
	TestEqual(TEXT("And clamps at the maximum rather than running away"),
		Input->GetChargeFraction(), 1.f);

	// Picking up the OTHER trigger mid-charge must not turn the release into an
	// element select either -- the release is checked before the modifiers.
	Input->SetMagicModifier(true);
	Listener->Clear();
	Input->ReleaseFace(EARPGInputFace::North);

	TestTrue(TEXT("Y still discharges at full charge"), Listener->Saw(TEXT("Discharge:0@1.00")));
	TestFalse(TEXT("Not an element select"), Listener->Saw(TEXT("MagicSelect:0")));

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGInputModifierPriorityTest,
	"ARPG.Input.Modal.DischargeModifierWinsWhenBothAreHeld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGInputModifierPriorityTest::RunTest(const FString& Parameters)
{
	using namespace ARPGInputTestUtils;

	FTestWorld Fixture;
	AActor* Host = SpawnHost(Fixture.World);
	UARPGModalInputComponent* Input = MakeInput(Host);

	UARPGModalInputTestListener* Listener = NewObject<UARPGModalInputTestListener>();
	Listener->Bind(Input);

	Input->SetMagicModifier(true);
	Input->SetDischargeModifier(true);
	Listener->Clear();

	Input->PressFace(EARPGInputFace::South);

	// Starting a charge is the more committed of the two, and readying an
	// element is a step on the way to it -- so the ambiguous grip resolves
	// towards what the player is further into doing.
	TestTrue(TEXT("RT wins: the press starts a charge"), Listener->Saw(TEXT("ChargeStarted:2")));
	TestFalse(TEXT("Rather than readying an element"), Listener->Saw(TEXT("MagicSelect:2")));

	// A second face button mid-charge is ignored rather than stealing the slot,
	// so a fumbled grip cannot silently swap which spell is about to come out.
	Listener->Clear();
	Input->PressFace(EARPGInputFace::East);

	TestEqual(TEXT("A second face button does not steal the charge"),
		Listener->Events.Num(), 0);
	TestEqual(TEXT("The original slot still owns it"),
		static_cast<int32>(Input->GetChargingSlot()),
		static_cast<int32>(EARPGInputFace::South));

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGInputCancelChargeTest,
	"ARPG.Input.Modal.CancellingAChargeDropsItWithoutFiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGInputCancelChargeTest::RunTest(const FString& Parameters)
{
	using namespace ARPGInputTestUtils;

	FTestWorld Fixture;
	AActor* Host = SpawnHost(Fixture.World);
	UARPGModalInputComponent* Input = MakeInput(Host);

	UARPGModalInputTestListener* Listener = NewObject<UARPGModalInputTestListener>();
	Listener->Bind(Input);

	Input->SetDischargeModifier(true);
	Input->PressFace(EARPGInputFace::East);
	Tick(Input, 0.5f);

	Listener->Clear();
	Input->CancelCharge();

	TestTrue(TEXT("Cancelling says so"), Listener->Saw(TEXT("DischargeCancelled")));
	TestFalse(TEXT("And nothing is charging afterwards"), Input->IsCharging());

	// The Godot original declared this signal and never emitted it, so a charge
	// begun before dying survived the respawn and fired on the first face
	// release afterwards. That is the bug this asserts is gone.
	Listener->Clear();
	Input->ReleaseFace(EARPGInputFace::East);

	TestFalse(TEXT("A cancelled charge does not fire when the button comes up"),
		Listener->Saw(TEXT("Discharge:3@0.33")));
	TestEqual(TEXT("The release is inert -- B has no release meaning of its own"),
		Listener->Events.Num(), 0);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGInputParryEdgeTest,
	"ARPG.Input.Modal.ParryReportsEdgesRatherThanRepeats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGInputParryEdgeTest::RunTest(const FString& Parameters)
{
	using namespace ARPGInputTestUtils;

	FTestWorld Fixture;
	AActor* Host = SpawnHost(Fixture.World);
	UARPGModalInputComponent* Input = MakeInput(Host);

	UARPGModalInputTestListener* Listener = NewObject<UARPGModalInputTestListener>();
	Listener->Bind(Input);

	Input->PressParry();
	Input->PressParry();
	Input->PressParry();

	// Edge-triggered, because the receiving side raises a guard on the press. A
	// repeat would restart the blend-in every frame the button was held, which
	// is a guard that never finishes coming up and so never parries.
	TestEqual(TEXT("Holding LB is one press, not many"),
		Listener->CountOf(TEXT("ParryPressed")), 1);
	TestTrue(TEXT("And the component knows it is held"), Input->IsParryHeld());

	Input->ReleaseParry();
	Input->ReleaseParry();

	TestEqual(TEXT("And one release"), Listener->CountOf(TEXT("ParryReleased")), 1);
	TestFalse(TEXT("No longer held"), Input->IsParryHeld());

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGLocomotionTierTest,
	"ARPG.Locomotion.WalkAndAttackScalingOutrankASprintTheyAskedFor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGLocomotionTierTest::RunTest(const FString& Parameters)
{
	using namespace ARPGInputTestUtils;

	FTestWorld Fixture;
	AActor* Host = SpawnHost(Fixture.World);
	UARPGLocomotionComponent* Locomotion =
		AddComponent<UARPGLocomotionComponent>(Host, TEXT("Locomotion"));

	Locomotion->WalkSpeed = 100.f;
	Locomotion->RunSpeed = 400.f;
	Locomotion->SprintSpeed = 800.f;
	Locomotion->SprintStaminaDrain = 0.f;

	TestEqual(TEXT("Standing still, a character runs"),
		static_cast<int32>(Locomotion->GetSpeedTier()), static_cast<int32>(EARPGSpeedTier::Run));
	TestEqual(TEXT("At the run speed"), Locomotion->GetCurrentSpeed(), 400.f);

	Locomotion->SetMoveMagnitude(1.f);
	Locomotion->ToggleSprint();
	TestTrue(TEXT("Sprint is a toggle, not a hold"), Locomotion->IsSprinting());
	TestEqual(TEXT("And it is faster"), Locomotion->GetCurrentSpeed(), 800.f);

	// A sprint ignores how far the stick is pushed: the tier the player asked
	// for should not depend on their thumb.
	TestTrue(TEXT("A sprint runs at full speed regardless of deflection"),
		Locomotion->ShouldIgnoreInputMagnitude());

	// A readied element forces a walk, and that outranks a sprint the player
	// explicitly asked for -- it is a restriction, not a preference.
	Locomotion->SetWalkForced(true);
	TestFalse(TEXT("Readying an element drops the sprint"), Locomotion->IsSprinting());
	TestEqual(TEXT("Down to a walk"), Locomotion->GetCurrentSpeed(), 100.f);

	Locomotion->ToggleSprint();
	TestFalse(TEXT("And a sprint cannot be started while it holds"),
		Locomotion->IsSprinting());

	Locomotion->SetWalkForced(false);
	Locomotion->ToggleSprint();
	TestTrue(TEXT("Discarding the element gives it back"), Locomotion->IsSprinting());

	// An attack scales the whole tier, and can forbid the sprint separately: a
	// slow attack that may still be sprint-cancelled and a fast one that may
	// not are both real, so the factor does not imply the permission.
	Locomotion->SetAttackMovement(0.25f, /*bAllowSprint=*/false);
	TestFalse(TEXT("An attack that forbids sprinting ends it"), Locomotion->IsSprinting());
	TestEqual(TEXT("And scales what is left"), Locomotion->GetCurrentSpeed(), 100.f);

	Locomotion->ClearAttackMovement();
	TestEqual(TEXT("Clearing restores the run"), Locomotion->GetCurrentSpeed(), 400.f);

	// Centring the stick ends a sprint. One that survived the release would
	// resume at full speed in a new direction on the next nudge.
	Locomotion->ToggleSprint();
	Locomotion->SetMoveMagnitude(0.f);
	TestFalse(TEXT("Letting go of the stick ends the sprint"), Locomotion->IsSprinting());

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGLocomotionStaminaTest,
	"ARPG.Locomotion.SprintEndsWhenStaminaRunsOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGLocomotionStaminaTest::RunTest(const FString& Parameters)
{
	using namespace ARPGInputTestUtils;

	FTestWorld Fixture;

	// A dummy, because this is the one locomotion rule that needs an ability
	// system to answer: everything else is arithmetic on authored numbers.
	AARPGCombatDummy* Dummy = Fixture.World->SpawnActor<AARPGCombatDummy>();
	UAbilitySystemComponent* ASC = Dummy->GetAbilitySystemComponent();
	TestNotNull(TEXT("The dummy has an ability system"), ASC);
	if (!ASC)
	{
		return false;
	}

	ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxStaminaAttribute(), 100.f);
	ASC->SetNumericAttributeBase(UARPGVitalSet::GetStaminaAttribute(), 10.f);

	UARPGLocomotionComponent* Locomotion =
		AddComponent<UARPGLocomotionComponent>(Dummy, TEXT("Locomotion"));
	Locomotion->RunSpeed = 400.f;
	Locomotion->SprintSpeed = 800.f;
	Locomotion->SprintStaminaDrain = 20.f; // half a second of sprint in the tank

	Locomotion->SetMoveMagnitude(1.f);
	Locomotion->ToggleSprint();
	TestTrue(TEXT("Sprinting starts"), Locomotion->IsSprinting());

	Tick(Locomotion, 0.25f);
	TestTrue(TEXT("Still going while there is stamina to pay with"),
		Locomotion->IsSprinting());
	TestTrue(TEXT("And it is being spent"),
		ASC->GetNumericAttribute(UARPGVitalSet::GetStaminaAttribute()) < 10.f);

	Tick(Locomotion, 1.f);

	TestFalse(TEXT("Running dry ends the sprint"), Locomotion->IsSprinting());
	TestEqual(TEXT("Back to a run"), Locomotion->GetCurrentSpeed(), 400.f);

	// NOT a cooldown. An exhausted character who pulls the stick again should
	// get a couple of steps out of whatever regenerated, not be told no.
	ASC->SetNumericAttributeBase(UARPGVitalSet::GetStaminaAttribute(), 50.f);
	Locomotion->ToggleSprint();
	TestTrue(TEXT("And they may start again the moment they can pay"),
		Locomotion->IsSprinting());

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGAutoSheatheTest,
	"ARPG.Weapon.AutoSheatheWaitsOutTheIdleDelay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGAutoSheatheTest::RunTest(const FString& Parameters)
{
	using namespace ARPGInputTestUtils;

	FTestWorld Fixture;
	AARPGCombatDummy* Dummy = Fixture.World->SpawnActor<AARPGCombatDummy>();

	UARPGWeaponComponent* Weapon = AddComponent<UARPGWeaponComponent>(Dummy, TEXT("Weapon"));
	Weapon->bAutoSheatheEnabled = true;
	Weapon->AutoSheatheDelay = 1.f;

	Weapon->SetDrawn(true);
	TestTrue(TEXT("Drawn"), Weapon->IsDrawn());

	Tick(Weapon, 0.5f);
	TestTrue(TEXT("Still out partway through the delay"), Weapon->IsDrawn());

	// Any combat activity restarts the countdown from zero.
	Weapon->NotifyCombatActivity();
	Tick(Weapon, 0.8f);
	TestTrue(TEXT("A swing partway through buys the full delay again"), Weapon->IsDrawn());

	Tick(Weapon, 0.5f);
	TestFalse(TEXT("And a full idle delay puts it away"), Weapon->IsDrawn());

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGAutoSheatheSuppressionTest,
	"ARPG.Weapon.AutoSheatheHoldsAtTheThresholdWhileTargeted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGAutoSheatheSuppressionTest::RunTest(const FString& Parameters)
{
	using namespace ARPGInputTestUtils;

	FTestWorld Fixture;
	AARPGCombatDummy* Dummy = Fixture.World->SpawnActor<AARPGCombatDummy>();

	UARPGWeaponComponent* Weapon = AddComponent<UARPGWeaponComponent>(Dummy, TEXT("Weapon"));
	Weapon->AutoSheatheDelay = 1.f;
	Weapon->SetDrawn(true);
	Weapon->SetAutoSheatheSuppressed(true);

	Tick(Weapon, 3.f);
	TestTrue(TEXT("Steel stays out while something is still aiming at you"),
		Weapon->IsDrawn());

	// THE DISTINCTION THIS TEST EXISTS FOR. Suppression HOLDS the timer at the
	// threshold rather than resetting it, so the weapon goes away on the very
	// next tick once the last enemy disengages -- not a full delay later.
	Weapon->SetAutoSheatheSuppressed(false);
	Tick(Weapon, 0.1f);

	TestFalse(TEXT("It goes away immediately once nothing is interested"),
		Weapon->IsDrawn());

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGAutoSheatheMidActionTest,
	"ARPG.Weapon.AutoSheatheNeverFiresMidBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGAutoSheatheMidActionTest::RunTest(const FString& Parameters)
{
	using namespace ARPGInputTestUtils;

	FTestWorld Fixture;
	AARPGCombatDummy* Dummy = Fixture.World->SpawnActor<AARPGCombatDummy>();

	UARPGWeaponComponent* Weapon = AddComponent<UARPGWeaponComponent>(Dummy, TEXT("Weapon"));
	Weapon->AutoSheatheDelay = 0.5f;
	Weapon->SetDrawn(true);

	UARPGParryComponent* Parry = AddComponent<UARPGParryComponent>(Dummy, TEXT("Parry"));
	Parry->BeginBlock();
	TestTrue(TEXT("Guard is up"), Parry->IsBlocking());

	// Holding a guard is not idling, and a block reports no per-frame activity of
	// its own -- so without this the weapon would sheathe itself out from behind
	// a shield the player is still holding up.
	Tick(Weapon, 2.f);
	TestTrue(TEXT("A held guard keeps the weapon out"), Weapon->IsDrawn());

	Parry->EndBlock();
	Tick(Weapon, 1.f);
	TestFalse(TEXT("And dropping it starts the clock"), Weapon->IsDrawn());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
