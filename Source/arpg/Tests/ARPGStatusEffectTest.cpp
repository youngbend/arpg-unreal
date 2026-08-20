// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "ARPGTestFixtures.h"

#if WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS

#include "ARPGCombatDummy.h"
#include "ARPGGameplayTags.h"
#include "ARPGOffenseSet.h"
#include "ARPGResistanceSet.h"
#include "ARPGStatusEffects.h"
#include "ARPGStatusResistanceComponent.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

/**
 * Status effects. The interesting assertions are the ones covering behaviour
 * that used to be GDScript: Wet's fire resistance and Burning removal, and
 * Weakened's damage reduction. Those are now pure GameplayEffect data, and these
 * tests are what prove the data says the same thing the script did.
 */
namespace ARPGStatusTestUtils
{
	using ARPGTest::FTestWorld;

	AARPGCombatDummy* SpawnDummy(UWorld* World, float Health = 100.f)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AARPGCombatDummy* Dummy = World->SpawnActor<AARPGCombatDummy>(
			AARPGCombatDummy::StaticClass(), FTransform::Identity, Params);

		UAbilitySystemComponent* ASC = Dummy->GetAbilitySystemComponent();
		ASC->InitAbilityActorInfo(Dummy, Dummy);
		ASC->AddLooseGameplayTag(TAG_Faction_Enemy, 1, EGameplayTagReplicationState::TagOnly);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxHealthAttribute(), Health);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetHealthAttribute(), Health);
		return Dummy;
	}

	void ApplyStatus(AARPGCombatDummy* Target, TSubclassOf<UGameplayEffect> EffectClass, int32 Times = 1)
	{
		UAbilitySystemComponent* ASC = Target->GetAbilitySystemComponent();
		for (int32 i = 0; i < Times; ++i)
		{
			FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
			const FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(EffectClass, 1.f, Context);
			ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data);
		}
	}

	float GetAttr(AARPGCombatDummy* Target, const FGameplayAttribute& Attribute)
	{
		return Target->GetAbilitySystemComponent()->GetNumericAttribute(Attribute);
	}

	int32 StackCount(AARPGCombatDummy* Target, TSubclassOf<UGameplayEffect> EffectClass)
	{
		return Target->GetAbilitySystemComponent()->GetGameplayEffectCount(EffectClass, nullptr);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGStatusEffectTest,
	"ARPG.Combat.StatusEffects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGStatusEffectTest::RunTest(const FString& Parameters)
{
	using namespace ARPGStatusTestUtils;

	FTestWorld TestWorld;
	UWorld* World = TestWorld.World;

	// --- Identity and stacking ------------------------------------------------
	{
		AARPGCombatDummy* Target = SpawnDummy(World);

		ApplyStatus(Target, UARPGStatusEffect_Burning::StaticClass());
		TestTrue(TEXT("burning grants its status tag"),
			Target->GetAbilitySystemComponent()->HasMatchingGameplayTag(TAG_Status_Burning));
		TestEqual(TEXT("burning starts at one stack"),
			StackCount(Target, UARPGStatusEffect_Burning::StaticClass()), 1);

		// Additive stacking, matching stack_behavior = 1 on burning.tres.
		ApplyStatus(Target, UARPGStatusEffect_Burning::StaticClass(), 3);
		TestEqual(TEXT("burning stacks additively"),
			StackCount(Target, UARPGStatusEffect_Burning::StaticClass()), 4);

		// Clamped at max_stacks = 5, not unbounded.
		ApplyStatus(Target, UARPGStatusEffect_Burning::StaticClass(), 5);
		TestEqual(TEXT("burning clamps at max stacks"),
			StackCount(Target, UARPGStatusEffect_Burning::StaticClass()), 5);
	}

	// --- Wet: the whole of wet.gd, as data ------------------------------------
	{
		AARPGCombatDummy* Target = SpawnDummy(World);

		TestEqual(TEXT("fire resistance starts at zero"),
			GetAttr(Target, UARPGResistanceSet::GetFireResistanceAttribute()), 0.f);

		ApplyStatus(Target, UARPGStatusEffect_Wet::StaticClass());
		TestEqual(TEXT("one wet stack grants 0.1 fire resistance"),
			GetAttr(Target, UARPGResistanceSet::GetFireResistanceAttribute()), 0.1f);

		// The case wet.gd needed a meta flag and a stack-changed hook to get
		// right: re-application must not compound the bonus, it must track the
		// stack count. Standing in water re-applies once a second, so a
		// compounding implementation ran away within seconds.
		ApplyStatus(Target, UARPGStatusEffect_Wet::StaticClass(), 2);
		TestEqual(TEXT("three wet stacks grant exactly 0.3, not a compounded value"),
			GetAttr(Target, UARPGResistanceSet::GetFireResistanceAttribute()), 0.3f);

		// Clamped at 3 stacks, so resistance stops climbing.
		ApplyStatus(Target, UARPGStatusEffect_Wet::StaticClass(), 4);
		TestEqual(TEXT("wet clamps at three stacks"),
			StackCount(Target, UARPGStatusEffect_Wet::StaticClass()), 3);
		TestEqual(TEXT("fire resistance stops at 0.3"),
			GetAttr(Target, UARPGResistanceSet::GetFireResistanceAttribute()), 0.3f);
	}

	// --- Wet strips Burning ---------------------------------------------------
	{
		AARPGCombatDummy* Target = SpawnDummy(World);
		UAbilitySystemComponent* ASC = Target->GetAbilitySystemComponent();

		ApplyStatus(Target, UARPGStatusEffect_Burning::StaticClass(), 3);
		TestTrue(TEXT("burning applied"), ASC->HasMatchingGameplayTag(TAG_Status_Burning));

		ApplyStatus(Target, UARPGStatusEffect_Wet::StaticClass());
		TestFalse(TEXT("wet removes burning entirely, not one stack"),
			ASC->HasMatchingGameplayTag(TAG_Status_Burning));
		TestEqual(TEXT("no burning stacks remain"),
			StackCount(Target, UARPGStatusEffect_Burning::StaticClass()), 0);
	}

	// --- Weakened: the whole of weakened.gd, as data --------------------------
	{
		AARPGCombatDummy* Target = SpawnDummy(World);

		TestEqual(TEXT("damage amp starts at identity"),
			GetAttr(Target, UARPGOffenseSet::GetDamageAmpMultiplierAttribute()), 1.f);

		ApplyStatus(Target, UARPGStatusEffect_Weakened::StaticClass());
		TestEqual(TEXT("weakened halves outgoing damage"),
			GetAttr(Target, UARPGOffenseSet::GetDamageAmpMultiplierAttribute()), 0.5f);
	}

	// --- A corpse cannot catch fire -------------------------------------------
	// Guards the bug CombatComponent::apply_status_effect documented: spread and
	// cloak apply statuses outside the damage pipeline, and a stack landing on a
	// corpse could never expire because ticking is itself gated on being alive.
	{
		AARPGCombatDummy* Target = SpawnDummy(World);
		Target->GetAbilitySystemComponent()->SetNumericAttributeBase(
			UARPGVitalSet::GetHealthAttribute(), 0.f);

		ApplyStatus(Target, UARPGStatusEffect_Burning::StaticClass());
		TestFalse(TEXT("a dead target cannot be set alight"),
			Target->GetAbilitySystemComponent()->HasMatchingGameplayTag(TAG_Status_Burning));
	}

	// --- Target immunity ------------------------------------------------------
	{
		AARPGCombatDummy* Target = SpawnDummy(World);

		UARPGStatusResistanceComponent* Resistance =
			NewObject<UARPGStatusResistanceComponent>(Target);
		Resistance->RegisterComponent();
		Resistance->Immunities.AddTag(TAG_Status_Burning);

		ApplyStatus(Target, UARPGStatusEffect_Burning::StaticClass());
		TestFalse(TEXT("an immune target is never set alight"),
			Target->GetAbilitySystemComponent()->HasMatchingGameplayTag(TAG_Status_Burning));

		// Immunity is per-status, not blanket.
		ApplyStatus(Target, UARPGStatusEffect_Wet::StaticClass());
		TestTrue(TEXT("immunity to burning does not block wet"),
			Target->GetAbilitySystemComponent()->HasMatchingGameplayTag(TAG_Status_Wet));
	}

	// --- Periodic damage ------------------------------------------------------
	// Burning ticks through the ordinary damage pipeline, so fire resistance
	// applies to a burn exactly as it does to a fireball. That is what makes Wet
	// actually protective rather than merely thematic.
	{
		AARPGCombatDummy* Target = SpawnDummy(World);
		ApplyStatus(Target, UARPGStatusEffect_Burning::StaticClass());

		TestWorld.Advance(1.2f);

		const float Health = GetAttr(Target, UARPGVitalSet::GetHealthAttribute());
		TestTrue(TEXT("burning deals periodic damage"), Health < 100.f);
		TestTrue(TEXT("burning does not deal more than one stack's worth per tick"),
			Health >= 89.f);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS
