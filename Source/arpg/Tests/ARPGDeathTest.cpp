// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "ARPGTestFixtures.h"

#if WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS

#include "ARPGCombatDummy.h"
#include "ARPGGameplayTags.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameplayEffect.h"

/**
 * Dying.
 *
 * WHAT THIS COVERS AND WHY IT DID NOT EXIST BEFORE. State.Dead was declared in
 * phase 0 and READ in seven places -- every offensive ability's
 * ActivationBlockedTags, the player action component's input gate, the
 * auto-sheathe countdown, the hit-stop multicast -- and granted by NOTHING. All
 * of that gating was therefore inert: a character at zero health kept attacking,
 * kept being targeted and kept flinching. The attribute set clamped health to
 * zero and returned.
 *
 * The crossing is now detected where it happens, in
 * UARPGVitalSet::PostGameplayEffectExecute, which is the only place that sees
 * every path health can move along. These cases pin the three properties that
 * makes worth having:
 *
 *   the tag actually arrives, so the gates gate;
 *   the EVENT fires exactly once per death, so a death reaction is an ability
 *     rather than something polled;
 *   healing back above zero lifts it, so a revive is not a special case.
 */
namespace ARPGDeathTestUtils
{
	using ARPGTest::FTestWorld;

	AARPGCombatDummy* SpawnDummy(UWorld* World, float Health)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AARPGCombatDummy* Dummy = World->SpawnActor<AARPGCombatDummy>(
			AARPGCombatDummy::StaticClass(), FTransform::Identity, Params);

		UAbilitySystemComponent* ASC = Dummy->GetAbilitySystemComponent();
		ASC->InitAbilityActorInfo(Dummy, Dummy);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxHealthAttribute(), Health);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetHealthAttribute(), Health);
		return Dummy;
	}

	/** An instant effect that moves Health by Delta. Negative kills. */
	void ApplyHealthDelta(UAbilitySystemComponent* ASC, float Delta)
	{
		UGameplayEffect* Effect = NewObject<UGameplayEffect>(
			GetTransientPackage(), FName(*FString::Printf(TEXT("HealthDelta_%d"), FMath::Rand())));
		Effect->DurationPolicy = EGameplayEffectDurationType::Instant;

		FGameplayModifierInfo Mod;
		Mod.Attribute = UARPGVitalSet::GetHealthAttribute();
		Mod.ModifierOp = EGameplayModOp::Additive;
		Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Delta));
		Effect->Modifiers.Add(Mod);

		ASC->ApplyGameplayEffectToSelf(Effect, 1.f, ASC->MakeEffectContext());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGDeathTagTest,
	"ARPG.Combat.Death.ReachingZeroGrantsTheTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGDeathTagTest::RunTest(const FString& Parameters)
{
	using namespace ARPGDeathTestUtils;
	FTestWorld Scope;

	AARPGCombatDummy* Dummy = SpawnDummy(Scope.World, 100.f);
	UAbilitySystemComponent* ASC = Dummy->GetAbilitySystemComponent();

	TestFalse(TEXT("Setup: alive"), ASC->HasMatchingGameplayTag(TAG_State_Dead));

	// A survivable hit changes nothing about the gates.
	ApplyHealthDelta(ASC, -40.f);
	TestEqual(TEXT("Damage lands"),
		ASC->GetNumericAttribute(UARPGVitalSet::GetHealthAttribute()), 60.f);
	TestFalse(TEXT("Still alive at 60"), ASC->HasMatchingGameplayTag(TAG_State_Dead));

	// Overkill. Health clamps at zero and the tag arrives.
	ApplyHealthDelta(ASC, -500.f);
	TestEqual(TEXT("Health floors at zero"),
		ASC->GetNumericAttribute(UARPGVitalSet::GetHealthAttribute()), 0.f);
	TestTrue(TEXT("State.Dead is granted"), ASC->HasMatchingGameplayTag(TAG_State_Dead));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGDeathEventTest,
	"ARPG.Combat.Death.DeathEventFiresOncePerDeath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGDeathEventTest::RunTest(const FString& Parameters)
{
	using namespace ARPGDeathTestUtils;
	FTestWorld Scope;

	AARPGCombatDummy* Dummy = SpawnDummy(Scope.World, 50.f);
	UAbilitySystemComponent* ASC = Dummy->GetAbilitySystemComponent();

	int32 DeathEvents = 0;
	ASC->AddGameplayEventTagContainerDelegate(
		FGameplayTagContainer(TAG_Event_Death),
		FGameplayEventTagMulticastDelegate::FDelegate::CreateLambda(
			[&DeathEvents](FGameplayTag, const FGameplayEventData*) { ++DeathEvents; }));

	ApplyHealthDelta(ASC, -50.f);
	TestTrue(TEXT("Dead"), ASC->HasMatchingGameplayTag(TAG_State_Dead));
	TestEqual(TEXT("Event.Death fired once"), DeathEvents, 1);

	// Hits still arriving from a swing already in flight must not re-raise it.
	// One death, however many blows land on the corpse.
	ApplyHealthDelta(ASC, -10.f);
	ApplyHealthDelta(ASC, -10.f);
	TestEqual(TEXT("Further hits on a corpse raise nothing"), DeathEvents, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGReviveTest,
	"ARPG.Combat.Death.HealingBackLiftsTheTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGReviveTest::RunTest(const FString& Parameters)
{
	using namespace ARPGDeathTestUtils;
	FTestWorld Scope;

	AARPGCombatDummy* Dummy = SpawnDummy(Scope.World, 40.f);
	UAbilitySystemComponent* ASC = Dummy->GetAbilitySystemComponent();

	ApplyHealthDelta(ASC, -40.f);
	TestTrue(TEXT("Dead"), ASC->HasMatchingGameplayTag(TAG_State_Dead));

	// A revive is healing, not a bespoke path -- which is the point of resolving
	// the state from the attribute rather than latching it.
	ApplyHealthDelta(ASC, 25.f);
	TestEqual(TEXT("Healed"),
		ASC->GetNumericAttribute(UARPGVitalSet::GetHealthAttribute()), 25.f);
	TestFalse(TEXT("State.Dead is lifted"), ASC->HasMatchingGameplayTag(TAG_State_Dead));

	// And the whole cycle repeats cleanly, so nothing about the first death is
	// still latched.
	ApplyHealthDelta(ASC, -25.f);
	TestTrue(TEXT("Dying again works"), ASC->HasMatchingGameplayTag(TAG_State_Dead));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS
