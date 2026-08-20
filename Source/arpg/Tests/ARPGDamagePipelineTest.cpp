// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "ARPGTestFixtures.h"

#if WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS

#include "ARPGCombatDummy.h"
#include "ARPGCombatLibrary.h"
#include "ARPGDamageGameplayEffect.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGGameplayEffectContext.h"
#include "ARPGGameplayTags.h"
#include "ARPGResistanceSet.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

/**
 * Locks the damage mitigation formula.
 *
 * These exist because phases 3 and 5 insert into the middle of that formula
 * (parry/block interception, then cloak absorption). The order of armor,
 * resistance and interception is what the Godot tuning was built against, and a
 * later insert at the wrong point changes every damage number in the game
 * without breaking a single compile.
 */
namespace ARPGDamageTestUtils
{
	using ARPGTest::FTestWorld;

	/** A damage type built in memory, so the test needs no content assets. */
	UARPGDamageTypeAsset* MakeDamageType(
		FGameplayTag TypeTag,
		const FGameplayTag& CategoryTag,
		const FGameplayAttribute& ResistanceAttribute,
		float DefaultPenetration = 0.f,
		float MinResistance = -1.f)
	{
		UARPGDamageTypeAsset* Type = NewObject<UARPGDamageTypeAsset>();
		Type->DamageTypeTag = TypeTag;
		if (CategoryTag.IsValid())
		{
			Type->Categories.AddTag(CategoryTag);
		}
		Type->ResistanceAttribute = ResistanceAttribute;
		Type->DefaultPenetration = DefaultPenetration;
		Type->MinResistance = MinResistance;
		return Type;
	}

	/**
	 * Spawns a dummy and initialises its ability system EXPLICITLY rather than
	 * relying on BeginPlay.
	 *
	 * A bare UWorld::CreateWorld world has no game mode and does not reliably
	 * dispatch BeginPlay to actors spawned into it, so leaning on the dummy's own
	 * BeginPlay left every attribute at its constructor default and every
	 * mitigation case silently reading zero. Doing the setup here keeps the test
	 * measuring the damage execution instead of the test harness's world
	 * lifecycle.
	 */
	AARPGCombatDummy* SpawnDummy(UWorld* World, FGameplayTag Faction,
		float MaxHealth, float BaseArmor,
		UARPGDamageTypeAsset* ResistType = nullptr, float ResistValue = 0.f)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AARPGCombatDummy* Dummy = World->SpawnActor<AARPGCombatDummy>(
			AARPGCombatDummy::StaticClass(), FTransform::Identity, Params);

		UAbilitySystemComponent* ASC = Dummy->GetAbilitySystemComponent();
		ASC->InitAbilityActorInfo(Dummy, Dummy);
		ASC->AddLooseGameplayTag(Faction, 1, EGameplayTagReplicationState::TagOnly);

		ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxHealthAttribute(), MaxHealth);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetHealthAttribute(), MaxHealth);
		ASC->SetNumericAttributeBase(UARPGResistanceSet::GetBaseArmorAttribute(), BaseArmor);

		if (ResistType && ResistType->ResistanceAttribute.IsValid())
		{
			ASC->SetNumericAttributeBase(ResistType->ResistanceAttribute, ResistValue);
		}

		return Dummy;
	}

	/** Applies one damage effect and returns the target's resulting health. */
	float ApplyDamage(AARPGCombatDummy* Source, AARPGCombatDummy* Target,
		UARPGDamageTypeAsset* DamageType, float Amount,
		float PenetrationOverride = -1.f, bool bCritical = false)
	{
		UAbilitySystemComponent* SourceASC = Source->GetAbilitySystemComponent();
		UAbilitySystemComponent* TargetASC = Target->GetAbilitySystemComponent();

		FGameplayEffectContextHandle ContextHandle = SourceASC->MakeEffectContext();
		if (FARPGGameplayEffectContext* Context =
				FARPGGameplayEffectContext::ExtractFrom(ContextHandle))
		{
			Context->DamageType = DamageType;
			Context->Penetration = PenetrationOverride;
			Context->bIsCritical = bCritical;
		}

		const FGameplayEffectSpecHandle SpecHandle = SourceASC->MakeOutgoingSpec(
			UARPGDamageGameplayEffect::StaticClass(), 1.f, ContextHandle);
		SpecHandle.Data->SetSetByCallerMagnitude(TAG_Data_Damage, Amount);

		SourceASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data, TargetASC);

		return TargetASC->GetNumericAttribute(UARPGVitalSet::GetHealthAttribute());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGDamageMitigationTest,
	"ARPG.Combat.DamageMitigation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGDamageMitigationTest::RunTest(const FString& Parameters)
{
	using namespace ARPGDamageTestUtils;

	FTestWorld TestWorld;
	UWorld* World = TestWorld.World;

	UARPGDamageTypeAsset* Physical = MakeDamageType(
		TAG_Damage_Physical, TAG_Damage_Category_Physical,
		UARPGResistanceSet::GetPhysicalResistanceAttribute());

	UARPGDamageTypeAsset* Fire = MakeDamageType(
		TAG_Damage_Fire, TAG_Damage_Category_Elemental,
		UARPGResistanceSet::GetFireResistanceAttribute());

	UARPGDamageTypeAsset* TrueDamage = MakeDamageType(
		TAG_Damage_Fall, TAG_Damage_Category_True,
		UARPGResistanceSet::GetFallResistanceAttribute());

	AARPGCombatDummy* Attacker = SpawnDummy(World, TAG_Faction_Player, 100.f, 0.f);

	// 0. Setup sanity. If these fail, the harness is wrong and every result
	//    below is meaningless -- worth distinguishing from a real formula bug.
	{
		AARPGCombatDummy* Probe = SpawnDummy(World, TAG_Faction_Enemy, 120.f, 7.f, Fire, 0.25f);
		UAbilitySystemComponent* ProbeASC = Probe->GetAbilitySystemComponent();
		TestEqual(TEXT("setup: health applied"),
			ProbeASC->GetNumericAttribute(UARPGVitalSet::GetHealthAttribute()), 120.f);
		TestEqual(TEXT("setup: armor applied"),
			ProbeASC->GetNumericAttribute(UARPGResistanceSet::GetBaseArmorAttribute()), 7.f);
		TestEqual(TEXT("setup: resistance applied"),
			ProbeASC->GetNumericAttribute(UARPGResistanceSet::GetFireResistanceAttribute()), 0.25f);
	}

	// 1. Baseline: no armor, no resistance.
	{
		AARPGCombatDummy* Target = SpawnDummy(World, TAG_Faction_Enemy, 100.f, 0.f);
		const float Health = ApplyDamage(Attacker, Target, Physical, 50.f);
		TestEqual(TEXT("50 damage, no mitigation"), Health, 50.f);
	}

	// 2. Flat armor applies to PHYSICAL, before resistance: 50 - 10 = 40.
	{
		AARPGCombatDummy* Target = SpawnDummy(World, TAG_Faction_Enemy, 100.f, 10.f);
		const float Health = ApplyDamage(Attacker, Target, Physical, 50.f);
		TestEqual(TEXT("armor 10 vs 50 physical"), Health, 60.f);
	}

	// 3. Armor must NOT apply to non-physical: fire ignores it entirely.
	{
		AARPGCombatDummy* Target = SpawnDummy(World, TAG_Faction_Enemy, 100.f, 10.f);
		const float Health = ApplyDamage(Attacker, Target, Fire, 50.f);
		TestEqual(TEXT("armor does not reduce fire"), Health, 50.f);
	}

	// 4. Resistance scales: 50 * (1 - 0.5) = 25.
	{
		AARPGCombatDummy* Target = SpawnDummy(World, TAG_Faction_Enemy, 100.f, 0.f, Fire, 0.5f);
		const float Health = ApplyDamage(Attacker, Target, Fire, 50.f);
		TestEqual(TEXT("0.5 fire resistance halves damage"), Health, 75.f);
	}

	// 5. Negative resistance is VULNERABILITY: 50 * 1.5 = 75.
	{
		AARPGCombatDummy* Target = SpawnDummy(World, TAG_Faction_Enemy, 100.f, 0.f, Fire, -0.5f);
		const float Health = ApplyDamage(Attacker, Target, Fire, 50.f);
		TestEqual(TEXT("-0.5 fire resistance amplifies damage"), Health, 25.f);
	}

	// 6. Order matters: armor first, THEN resistance. (50 - 10) * 0.5 = 20.
	//    If resistance ran first it would be 50*0.5 - 10 = 15, i.e. health 85.
	{
		AARPGCombatDummy* Target = SpawnDummy(World, TAG_Faction_Enemy, 100.f, 10.f, Physical, 0.5f);
		const float Health = ApplyDamage(Attacker, Target, Physical, 50.f);
		TestEqual(TEXT("armor applies before resistance"), Health, 80.f);
	}

	// 7. Penetration reduces effective resistance: 0.5 * (1 - 0.5) = 0.25.
	{
		AARPGCombatDummy* Target = SpawnDummy(World, TAG_Faction_Enemy, 100.f, 0.f, Fire, 0.5f);
		const float Health = ApplyDamage(Attacker, Target, Fire, 50.f, /*Penetration=*/0.5f);
		TestEqual(TEXT("0.5 penetration halves resistance"), Health, 62.5f);
	}

	// 8. Damage.Category.True bypasses resistance entirely.
	{
		AARPGCombatDummy* Target = SpawnDummy(World, TAG_Faction_Enemy, 100.f, 0.f, TrueDamage, 0.9f);
		const float Health = ApplyDamage(Attacker, Target, TrueDamage, 50.f);
		TestEqual(TEXT("true damage ignores resistance"), Health, 50.f);
	}

	// 9. Full resistance is immunity.
	{
		AARPGCombatDummy* Target = SpawnDummy(World, TAG_Faction_Enemy, 100.f, 0.f, Fire, 1.f);
		const float Health = ApplyDamage(Attacker, Target, Fire, 50.f);
		TestEqual(TEXT("1.0 resistance is immunity"), Health, 100.f);
	}

	// 10. Health clamps at 0 rather than going negative.
	{
		AARPGCombatDummy* Target = SpawnDummy(World, TAG_Faction_Enemy, 100.f, 0.f);
		const float Health = ApplyDamage(Attacker, Target, Physical, 500.f);
		TestEqual(TEXT("overkill clamps to zero"), Health, 0.f);
	}

	// 11. Armor cannot turn a hit into healing.
	{
		AARPGCombatDummy* Target = SpawnDummy(World, TAG_Faction_Enemy, 100.f, 999.f);
		const float Health = ApplyDamage(Attacker, Target, Physical, 50.f);
		TestEqual(TEXT("armor exceeding damage deals zero, not negative"), Health, 100.f);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGFactionHostilityTest,
	"ARPG.Combat.FactionHostility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGFactionHostilityTest::RunTest(const FString& Parameters)
{
	const FGameplayTag Player  = TAG_Faction_Player;
	const FGameplayTag Ally    = TAG_Faction_Ally;
	const FGameplayTag Enemy   = TAG_Faction_Enemy;
	const FGameplayTag Neutral = TAG_Faction_Neutral;

	// Neutral is hostile to nobody, in either position.
	TestFalse(TEXT("neutral vs player"),  UARPGCombatLibrary::AreFactionsHostile(Neutral, Player));
	TestFalse(TEXT("player vs neutral"),  UARPGCombatLibrary::AreFactionsHostile(Player, Neutral));
	TestFalse(TEXT("neutral vs enemy"),   UARPGCombatLibrary::AreFactionsHostile(Neutral, Enemy));
	TestFalse(TEXT("neutral vs neutral"), UARPGCombatLibrary::AreFactionsHostile(Neutral, Neutral));

	// Player and Ally are one side.
	TestFalse(TEXT("player vs ally"),   UARPGCombatLibrary::AreFactionsHostile(Player, Ally));
	TestFalse(TEXT("player vs player"), UARPGCombatLibrary::AreFactionsHostile(Player, Player));
	TestFalse(TEXT("ally vs ally"),     UARPGCombatLibrary::AreFactionsHostile(Ally, Ally));

	// Enemy is the other. This is also what stops a self-fired projectile
	// killing its own caster.
	TestTrue(TEXT("player vs enemy"), UARPGCombatLibrary::AreFactionsHostile(Player, Enemy));
	TestTrue(TEXT("enemy vs player"), UARPGCombatLibrary::AreFactionsHostile(Enemy, Player));
	TestTrue(TEXT("ally vs enemy"),   UARPGCombatLibrary::AreFactionsHostile(Ally, Enemy));

	TestFalse(TEXT("enemy vs enemy"), UARPGCombatLibrary::AreFactionsHostile(Enemy, Enemy));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS
