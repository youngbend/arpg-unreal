// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "ARPGTestFixtures.h"

#if WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS

#include "ARPGCombatDummy.h"
#include "ARPGDamageGameplayEffect.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGGameplayEffectContext.h"
#include "ARPGGameplayTags.h"
#include "ARPGParryComponent.h"
#include "ARPGPoiseComponent.h"
#include "ARPGResistanceSet.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

/**
 * Poise, flinch tiers, stance break and guard interception.
 *
 * The assertions worth reading are the ones pinning behaviour that is easy to
 * "simplify" into something that feels completely different: flinch tiers keying
 * on a single blow rather than the running total, flinches not resetting the
 * meter, and hyperarmor covering the flinches but NOT the break.
 */
namespace ARPGPoiseTestUtils
{
	using ARPGTest::FTestWorld;

	using ARPGTest::TickComponents;

	struct FCombatant
	{
		AARPGCombatDummy* Actor = nullptr;
		UARPGPoiseComponent* Poise = nullptr;
		UARPGParryComponent* Parry = nullptr;

		UAbilitySystemComponent* ASC() const { return Actor->GetAbilitySystemComponent(); }
		float Health() const { return ASC()->GetNumericAttribute(UARPGVitalSet::GetHealthAttribute()); }
		float Stamina() const { return ASC()->GetNumericAttribute(UARPGVitalSet::GetStaminaAttribute()); }
	};

	FCombatant Spawn(UWorld* World, FGameplayTag Faction, const FVector& Location = FVector::ZeroVector)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		FCombatant C;
		C.Actor = World->SpawnActor<AARPGCombatDummy>(
			AARPGCombatDummy::StaticClass(), FTransform(Location), Params);

		UAbilitySystemComponent* ASC = C.Actor->GetAbilitySystemComponent();
		ASC->InitAbilityActorInfo(C.Actor, C.Actor);
		ASC->AddLooseGameplayTag(Faction, 1, EGameplayTagReplicationState::TagOnly);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxHealthAttribute(), 100.f);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetHealthAttribute(), 100.f);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxPoiseAttribute(), 100.f);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetPoiseAttribute(), 0.f);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxStaminaAttribute(), 100.f);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetStaminaAttribute(), 100.f);

		C.Poise = NewObject<UARPGPoiseComponent>(C.Actor);
		C.Poise->RegisterComponent();

		C.Parry = NewObject<UARPGParryComponent>(C.Actor);
		C.Parry->RegisterComponent();

		// Stands in for BeginPlay, which this world does not dispatch.
		C.Poise->EnsureSubscribed();

		return C;
	}

	UARPGDamageTypeAsset* MakePhysical()
	{
		UARPGDamageTypeAsset* Type = NewObject<UARPGDamageTypeAsset>();
		Type->DamageTypeTag = TAG_Damage_Physical;
		Type->Categories.AddTag(TAG_Damage_Category_Physical);
		Type->ResistanceAttribute = UARPGResistanceSet::GetPhysicalResistanceAttribute();
		return Type;
	}

	void Strike(const FCombatant& Attacker, const FCombatant& Target,
		UARPGDamageTypeAsset* DamageType, float Damage, float PoiseDamage, bool bUnblockable = false)
	{
		UAbilitySystemComponent* SourceASC = Attacker.ASC();

		FGameplayEffectContextHandle ContextHandle = SourceASC->MakeEffectContext();
		if (FARPGGameplayEffectContext* Context =
				FARPGGameplayEffectContext::ExtractFrom(ContextHandle))
		{
			Context->DamageType = DamageType;
			Context->PoiseDamage = PoiseDamage;
			Context->bUnblockable = bUnblockable;
		}

		const FGameplayEffectSpecHandle Spec = SourceASC->MakeOutgoingSpec(
			UARPGDamageGameplayEffect::StaticClass(), 1.f, ContextHandle);
		Spec.Data->SetSetByCallerMagnitude(TAG_Data_Damage, Damage);

		SourceASC->ApplyGameplayEffectSpecToTarget(*Spec.Data, Target.ASC());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPoiseTest,
	"ARPG.Combat.Poise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPoiseTest::RunTest(const FString& Parameters)
{
	using namespace ARPGPoiseTestUtils;

	FTestWorld TestWorld;
	UWorld* World = TestWorld.World;

	// Max poise 100, so thresholds are 8 (light) and 15 (heavy).

	// --- Flinch tiers key on a SINGLE contribution ---------------------------
	{
		FCombatant C = Spawn(World, TAG_Faction_Enemy);

		TestEqual(TEXT("a small blow causes no flinch"),
			C.Poise->ApplyPoiseDamage(5.f), EARPGPoiseResult::None);
		TestEqual(TEXT("light threshold reached"),
			C.Poise->ApplyPoiseDamage(8.f), EARPGPoiseResult::LightFlinch);
		TestEqual(TEXT("heavy wins outright over light"),
			C.Poise->ApplyPoiseDamage(20.f), EARPGPoiseResult::HeavyFlinch);

		// Neither tier resets the meter -- accumulation continues toward a break.
		TestEqual(TEXT("flinches do not reset the meter"), C.Poise->GetCurrentPoise(), 33.f);

		// And the tier is about THIS blow, not the total: at 33 accumulated, a
		// 5-point tap is still beneath the light threshold. Testing the running
		// total instead would make every late-fight tap stagger.
		TestEqual(TEXT("tier ignores how full the meter already is"),
			C.Poise->ApplyPoiseDamage(5.f), EARPGPoiseResult::None);
	}

	// --- Stance break ---------------------------------------------------------
	{
		FCombatant C = Spawn(World, TAG_Faction_Enemy);

		C.Poise->ApplyPoiseDamage(90.f);
		TestEqual(TEXT("meter fills toward the break"), C.Poise->GetCurrentPoise(), 90.f);

		TestEqual(TEXT("topping out breaks stance"),
			C.Poise->ApplyPoiseDamage(10.f), EARPGPoiseResult::StanceBreak);
		TestEqual(TEXT("break empties the meter"), C.Poise->GetCurrentPoise(), 0.f);
		TestTrue(TEXT("break opens an immunity window"), C.Poise->IsBreakImmune());

		// Immune to ACCUMULATION, not merely to breaking again -- otherwise the
		// meter would refill during recovery and break the instant it lapsed.
		TestEqual(TEXT("no poise accumulates during the immunity window"),
			C.Poise->ApplyPoiseDamage(50.f), EARPGPoiseResult::None);
		TestEqual(TEXT("meter stays empty while immune"), C.Poise->GetCurrentPoise(), 0.f);

		TickComponents({C.Poise}, 1.6f);
		TestFalse(TEXT("immunity lapses"), C.Poise->IsBreakImmune());
		TestEqual(TEXT("poise accumulates again afterwards"),
			C.Poise->ApplyPoiseDamage(20.f), EARPGPoiseResult::HeavyFlinch);
	}

	// --- Hyperarmor covers flinches but NOT the break -------------------------
	{
		FCombatant C = Spawn(World, TAG_Faction_Enemy);
		C.ASC()->AddLooseGameplayTag(TAG_State_Hyperarmor, 1, EGameplayTagReplicationState::TagOnly);

		TestEqual(TEXT("hyperarmor ignores a heavy flinch"),
			C.Poise->ApplyPoiseDamage(20.f), EARPGPoiseResult::None);
		TestEqual(TEXT("but the poise still accumulates"), C.Poise->GetCurrentPoise(), 20.f);

		// The part that keeps hyperarmor from being an unanswerable defence.
		TestEqual(TEXT("hyperarmor does NOT prevent a stance break"),
			C.Poise->ApplyPoiseDamage(80.f), EARPGPoiseResult::StanceBreak);
	}

	// --- Hold, then drain -----------------------------------------------------
	{
		FCombatant C = Spawn(World, TAG_Faction_Enemy);
		C.Poise->HoldTime = 0.3f;
		C.Poise->DrainRate = 50.f;

		C.Poise->ApplyPoiseDamage(50.f);

		// Holds first. Without the hold, a flurry could never reach the top
		// because the meter would bleed away between blows.
		TickComponents({C.Poise}, 0.2f);
		TestEqual(TEXT("meter holds before draining"), C.Poise->GetCurrentPoise(), 50.f);

		TickComponents({C.Poise}, 0.6f);
		TestTrue(TEXT("meter drains after the hold"), C.Poise->GetCurrentPoise() < 50.f);
	}

	// --- Guard interception ---------------------------------------------------
	UARPGDamageTypeAsset* Physical = MakePhysical();

	{
		FCombatant Attacker = Spawn(World, TAG_Faction_Player, FVector(0, 0, 0));
		FCombatant Defender = Spawn(World, TAG_Faction_Enemy, FVector(200, 0, 0));

		// Guard raised but still blending in: not even a block yet.
		Defender.Parry->BeginBlock();
		Strike(Attacker, Defender, Physical, 50.f, 0.f);
		TestEqual(TEXT("a hit during blend-in is not mitigated"), Defender.Health(), 50.f);
	}

	{
		FCombatant Attacker = Spawn(World, TAG_Faction_Player, FVector(0, 200, 0));
		FCombatant Defender = Spawn(World, TAG_Faction_Enemy, FVector(200, 200, 0));

		Defender.Parry->BeginBlock();
		TickComponents({Defender.Parry}, 0.15f); // past blend-in, inside the parry window

		Strike(Attacker, Defender, Physical, 50.f, 0.f);
		// 95% reduction -> 2.5 damage.
		TestEqual(TEXT("a parry stops 95% of the damage"), Defender.Health(), 97.5f);
		TestTrue(TEXT("a parry grants empowered"), Defender.Parry->IsEmpowered());

		// One guard press yields at most one parry.
		Strike(Attacker, Defender, Physical, 50.f, 0.f);
		TestEqual(TEXT("the second hit is only blocked, not parried again"),
			Defender.Health(), 72.5f);
	}

	{
		FCombatant Attacker = Spawn(World, TAG_Faction_Player, FVector(0, 400, 0));
		FCombatant Defender = Spawn(World, TAG_Faction_Enemy, FVector(200, 400, 0));

		Defender.Parry->BeginBlock();
		TickComponents({Defender.Parry}, 0.5f); // past the parry window: plain block

		Strike(Attacker, Defender, Physical, 50.f, 0.f);
		TestEqual(TEXT("a plain block stops 50%"), Defender.Health(), 75.f);

		// Stamina is charged for what the block actually stopped: 25 * 0.2 = 5.
		TestEqual(TEXT("blocking costs stamina proportional to damage stopped"),
			Defender.Stamina(), 95.f);
	}

	// --- Unblockable bypasses the guard entirely ------------------------------
	{
		FCombatant Attacker = Spawn(World, TAG_Faction_Player, FVector(0, 600, 0));
		FCombatant Defender = Spawn(World, TAG_Faction_Enemy, FVector(200, 600, 0));

		Defender.Parry->BeginBlock();
		TickComponents({Defender.Parry}, 0.15f); // squarely inside the parry window

		Strike(Attacker, Defender, Physical, 50.f, 0.f, /*bUnblockable=*/true);
		TestEqual(TEXT("unblockable ignores a perfect parry"), Defender.Health(), 50.f);
		TestFalse(TEXT("and does not consume the parry into empowered"),
			Defender.Parry->IsEmpowered());
	}

	// --- Defensive poise goes to the ATTACKER ---------------------------------
	{
		FCombatant Attacker = Spawn(World, TAG_Faction_Player, FVector(0, 800, 0));
		FCombatant Defender = Spawn(World, TAG_Faction_Enemy, FVector(200, 800, 0));

		Defender.Parry->BeginBlock();
		TickComponents({Defender.Parry}, 0.15f);

		Strike(Attacker, Defender, Physical, 50.f, /*PoiseDamage=*/30.f);

		TestEqual(TEXT("a parry pushes poise onto the attacker"),
			Attacker.Poise->GetCurrentPoise(), Attacker.Poise->ParryPoiseDamage);
		TestEqual(TEXT("and the defender's own meter is untouched"),
			Defender.Poise->GetCurrentPoise(), 0.f);
	}

	// --- An unintercepted hit fills the victim's meter -------------------------
	{
		FCombatant Attacker = Spawn(World, TAG_Faction_Player, FVector(0, 1000, 0));
		FCombatant Defender = Spawn(World, TAG_Faction_Enemy, FVector(200, 1000, 0));

		Strike(Attacker, Defender, Physical, 50.f, /*PoiseDamage=*/30.f);

		TestEqual(TEXT("an unguarded hit fills the victim's meter"),
			Defender.Poise->GetCurrentPoise(), 30.f);
		TestEqual(TEXT("and leaves the attacker's alone"),
			Attacker.Poise->GetCurrentPoise(), 0.f);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS
