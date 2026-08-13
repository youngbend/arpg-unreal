// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ARPGCombatDummy.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGGameplayTags.h"
#include "ARPGHitboxComponent.h"
#include "ARPGHurtboxComponent.h"
#include "ARPGResistanceSet.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

/**
 * Covers the hitbox contract that survived the port from Godot's
 * HitboxComponent: one hit per target per activation, faction filtering, and
 * invincibility frames.
 *
 * Detection itself changed -- swept traces instead of Area3D overlap events --
 * which is exactly why this exists. The behaviour above is what callers depend
 * on, and it has to hold regardless of how contact is discovered.
 */
namespace ARPGHitboxTestUtils
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

	/**
	 * As in the damage test, the ability system is initialised explicitly rather
	 * than via BeginPlay -- a bare created world does not reliably dispatch it.
	 */
	AARPGCombatDummy* SpawnDummy(UWorld* World, const FVector& Location,
		FGameplayTag Faction, float Health, float InvincibilityDuration = 0.f)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AARPGCombatDummy* Dummy = World->SpawnActor<AARPGCombatDummy>(
			AARPGCombatDummy::StaticClass(), FTransform(Location), Params);

		UAbilitySystemComponent* ASC = Dummy->GetAbilitySystemComponent();
		ASC->InitAbilityActorInfo(Dummy, Dummy);
		ASC->AddLooseGameplayTag(Faction, 1, EGameplayTagReplicationState::TagOnly);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxHealthAttribute(), Health);
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetHealthAttribute(), Health);

		if (UARPGHurtboxComponent* Hurtbox = Dummy->FindComponentByClass<UARPGHurtboxComponent>())
		{
			Hurtbox->InvincibilityDuration = InvincibilityDuration;
		}

		return Dummy;
	}

	/** Attaches a live hitbox to an actor at runtime, so no test-only UCLASS is needed. */
	UARPGHitboxComponent* AttachHitbox(AActor* Owner, UARPGDamageTypeAsset* DamageType, float Damage)
	{
		UARPGHitboxComponent* Hitbox = NewObject<UARPGHitboxComponent>(Owner);
		Hitbox->RegisterComponent();
		Hitbox->AttachToComponent(Owner->GetRootComponent(),
			FAttachmentTransformRules::KeepRelativeTransform);
		Hitbox->DamageType = DamageType;
		Hitbox->BaseDamage = Damage;
		Hitbox->TraceRadius = 50.f;
		return Hitbox;
	}

	float GetHealth(AARPGCombatDummy* Dummy)
	{
		return Dummy->GetAbilitySystemComponent()->GetNumericAttribute(
			UARPGVitalSet::GetHealthAttribute());
	}

	/** Drives one hitbox tick with the component parked at Location. */
	void TickAt(UARPGHitboxComponent* Hitbox, const FVector& Location, float DeltaTime = 0.016f)
	{
		Hitbox->SetWorldLocation(Location);
		Hitbox->TickComponent(DeltaTime, LEVELTICK_All, nullptr);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGHitboxDetectionTest,
	"ARPG.Combat.HitboxDetection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGHitboxDetectionTest::RunTest(const FString& Parameters)
{
	using namespace ARPGHitboxTestUtils;

	FTestWorld TestWorld;
	UWorld* World = TestWorld.World;

	UARPGDamageTypeAsset* Physical = NewObject<UARPGDamageTypeAsset>();
	Physical->DamageTypeTag = TAG_Damage_Physical;
	Physical->Categories.AddTag(TAG_Damage_Category_Physical);
	Physical->ResistanceAttribute = UARPGResistanceSet::GetPhysicalResistanceAttribute();

	const FVector TargetLocation(0.f, 0.f, 0.f);

	// --- Detection, and one hit per activation -------------------------------
	{
		AARPGCombatDummy* Attacker = SpawnDummy(World, FVector(-400.f, 0.f, 0.f), TAG_Faction_Player, 100.f);
		AARPGCombatDummy* Target   = SpawnDummy(World, TargetLocation, TAG_Faction_Enemy, 100.f);

		UARPGHitboxComponent* Hitbox = AttachHitbox(Attacker, Physical, 25.f);
		Hitbox->SetSourceActor(Attacker);

		// Approach the target WITHOUT reaching it. Every tick sweeps from the
		// previous position to the current one, so the intermediate position has
		// to stay short of contact -- parking the hitbox somewhere distant
		// instead would sweep a line straight THROUGH the target and legitimately
		// hit it, which is the tunnelling protection doing its job rather than a
		// bug. (The first draft of this test made exactly that mistake.)
		Hitbox->ActivateHitbox();
		TickAt(Hitbox, FVector(-300.f, 0.f, 0.f));
		TestEqual(TEXT("approach short of contact deals nothing"), GetHealth(Target), 100.f);

		// Swept onto the target.
		TickAt(Hitbox, TargetLocation);
		TestEqual(TEXT("sweep onto target deals damage"), GetHealth(Target), 75.f);

		// Still overlapping, same activation -- must NOT hit again. This is the
		// property that stops a swing shredding a target it happens to rest in.
		TickAt(Hitbox, TargetLocation);
		TickAt(Hitbox, TargetLocation);
		TestEqual(TEXT("same activation hits once only"), GetHealth(Target), 75.f);

		// A fresh activation is a fresh swing, so it may hit again.
		Hitbox->DeactivateHitbox();
		Hitbox->ActivateHitbox();
		TickAt(Hitbox, TargetLocation);
		TestEqual(TEXT("new activation hits again"), GetHealth(Target), 50.f);

		// Disarmed hitboxes do nothing.
		Hitbox->DeactivateHitbox();
		TickAt(Hitbox, TargetLocation);
		TestEqual(TEXT("deactivated hitbox deals nothing"), GetHealth(Target), 50.f);
	}

	// --- Tunnelling ----------------------------------------------------------
	// The reason detection is a sweep rather than an overlap volume. A fast
	// enough swing moves further in one frame than the target is wide; an
	// overlap test samples discrete positions and finds nothing on either side
	// of it, so the hit is silently lost. A sweep traces the gap itself.
	{
		AARPGCombatDummy* Attacker = SpawnDummy(World, FVector(-1000.f, 800.f, 0.f), TAG_Faction_Player, 100.f);
		AARPGCombatDummy* Target   = SpawnDummy(World, FVector(0.f, 800.f, 0.f), TAG_Faction_Enemy, 100.f);

		UARPGHitboxComponent* Hitbox = AttachHitbox(Attacker, Physical, 25.f);
		Hitbox->SetSourceActor(Attacker);

		Hitbox->SetWorldLocation(FVector(-1000.f, 800.f, 0.f));
		Hitbox->ActivateHitbox();

		// One frame, straight past the target and out the far side. Neither
		// endpoint overlaps it.
		TickAt(Hitbox, FVector(1000.f, 800.f, 0.f));

		TestEqual(TEXT("fast pass-through still connects (no tunnelling)"),
			GetHealth(Target), 75.f);
	}

	// --- Faction filter -------------------------------------------------------
	{
		AARPGCombatDummy* Attacker = SpawnDummy(World, FVector(-400.f, 200.f, 0.f), TAG_Faction_Player, 100.f);
		AARPGCombatDummy* Friendly = SpawnDummy(World, FVector(0.f, 200.f, 0.f), TAG_Faction_Player, 100.f);

		UARPGHitboxComponent* Hitbox = AttachHitbox(Attacker, Physical, 25.f);
		Hitbox->SetSourceActor(Attacker);
		Hitbox->ActivateHitbox();
		TickAt(Hitbox, FVector(0.f, 200.f, 0.f));

		TestEqual(TEXT("player hitbox does not damage a player-faction target"),
			GetHealth(Friendly), 100.f);

		// The same hitbox with filtering off is the environmental-hazard case.
		Hitbox->bIgnoreFactionFilter = true;
		Hitbox->DeactivateHitbox();
		Hitbox->ActivateHitbox();
		TickAt(Hitbox, FVector(0.f, 200.f, 0.f));

		TestEqual(TEXT("bIgnoreFactionFilter bypasses the check"), GetHealth(Friendly), 75.f);
	}

	// --- Invincibility frames -------------------------------------------------
	{
		AARPGCombatDummy* Attacker = SpawnDummy(World, FVector(-400.f, 400.f, 0.f), TAG_Faction_Player, 100.f);
		AARPGCombatDummy* Target   = SpawnDummy(World, FVector(0.f, 400.f, 0.f), TAG_Faction_Enemy, 100.f, /*IFrames=*/5.f);

		UARPGHitboxComponent* Hitbox = AttachHitbox(Attacker, Physical, 25.f);
		Hitbox->SetSourceActor(Attacker);

		Hitbox->ActivateHitbox();
		TickAt(Hitbox, FVector(0.f, 400.f, 0.f));
		TestEqual(TEXT("first hit lands"), GetHealth(Target), 75.f);

		// A brand-new activation would normally hit, but i-frames are still up.
		// This is what makes dodging through an attack work.
		Hitbox->DeactivateHitbox();
		Hitbox->ActivateHitbox();
		TickAt(Hitbox, FVector(0.f, 400.f, 0.f));
		TestEqual(TEXT("i-frames block a second activation"), GetHealth(Target), 75.f);
	}

	// --- Self-hit suppression -------------------------------------------------
	// A hazard spawned underfoot must not hit its own source on the activation
	// that created it, even with faction filtering disabled.
	{
		AARPGCombatDummy* Caster = SpawnDummy(World, FVector(0.f, 600.f, 0.f), TAG_Faction_Player, 100.f);

		UARPGHitboxComponent* Hitbox = AttachHitbox(Caster, Physical, 25.f);
		Hitbox->SetSourceActor(Caster);
		Hitbox->bIgnoreFactionFilter = true;

		// Armed while already overlapping the caster.
		Hitbox->SetWorldLocation(FVector(0.f, 600.f, 0.f));
		Hitbox->ActivateHitbox();
		TickAt(Hitbox, FVector(0.f, 600.f, 0.f));

		TestEqual(TEXT("hazard does not hit its own source on spawn"), GetHealth(Caster), 100.f);
	}

	return true;
}

/**
 * The elemental rider: an imbued swing's second, independently mitigated half.
 *
 * The property under test is the one the whole design turns on -- ONE contact
 * resolving through the mitigation pipeline TWICE, with a different damage type
 * each time. Armour answers for the steel and resistance for the fire, and they
 * are not the same number; a single folded damage value could only ever be right
 * for one of them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGHitboxElementalRiderTest,
	"ARPG.Combat.ElementalRiderIsMitigatedSeparately",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGHitboxElementalRiderTest::RunTest(const FString& Parameters)
{
	using namespace ARPGHitboxTestUtils;

	FTestWorld TestWorld;
	UWorld* World = TestWorld.World;

	UARPGDamageTypeAsset* Physical = NewObject<UARPGDamageTypeAsset>();
	Physical->DamageTypeTag = TAG_Damage_Physical;
	Physical->Categories.AddTag(TAG_Damage_Category_Physical);
	Physical->ResistanceAttribute = UARPGResistanceSet::GetPhysicalResistanceAttribute();

	UARPGDamageTypeAsset* Fire = NewObject<UARPGDamageTypeAsset>();
	Fire->DamageTypeTag = TAG_Damage_Fire;
	Fire->Categories.AddTag(TAG_Damage_Category_Elemental);
	Fire->ResistanceAttribute = UARPGResistanceSet::GetFireResistanceAttribute();

	/** Half-resists steel, does not resist fire at all. */
	auto SpawnArmouredTarget = [&World](const FVector& Location, float IFrames) -> AARPGCombatDummy*
	{
		AARPGCombatDummy* Target = SpawnDummy(World, Location, TAG_Faction_Enemy, 500.f, IFrames);
		UAbilitySystemComponent* ASC = Target->GetAbilitySystemComponent();
		ASC->SetNumericAttributeBase(UARPGResistanceSet::GetPhysicalResistanceAttribute(), 0.5f);
		ASC->SetNumericAttributeBase(UARPGResistanceSet::GetFireResistanceAttribute(), 0.f);
		return Target;
	};

	auto CoatWithFire = [&Fire](UARPGHitboxComponent* Hitbox, float Damage)
	{
		FARPGElementalRider Rider;
		Rider.BaseDamage = Damage;
		Rider.DamageType = Fire;
		Rider.MagicElementTag = TAG_Element_Fire;
		Hitbox->SetElementalRider(Rider);
	};

	// --- Two damage types, two resistance lookups, one contact ----------------
	{
		const FVector TargetLocation(0.f, 0.f, 0.f);
		AARPGCombatDummy* Attacker = SpawnDummy(World, FVector(-400.f, 0.f, 0.f), TAG_Faction_Player, 100.f);
		AARPGCombatDummy* Target = SpawnArmouredTarget(TargetLocation, /*IFrames=*/0.f);

		UARPGHitboxComponent* Hitbox = AttachHitbox(Attacker, Physical, 100.f);
		Hitbox->SetSourceActor(Attacker);
		CoatWithFire(Hitbox, 40.f);

		Hitbox->ActivateHitbox();
		TickAt(Hitbox, FVector(-300.f, 0.f, 0.f));
		TestEqual(TEXT("approach short of contact deals nothing"), GetHealth(Target), 500.f);

		TickAt(Hitbox, TargetLocation);

		// 100 steel halved by armour to 50, and 40 fire that the armour has no
		// answer for. Folding them would have produced 140 halved, or 140 whole.
		TestEqual(TEXT("both halves land, each mitigated by its own resistance"),
			GetHealth(Target), 500.f - 50.f - 40.f);
	}

	// --- The rider survives the target's i-frames ------------------------------
	// This is exactly why the coating rides the weapon's hitbox instead of being
	// a second hitbox component tracing the same arc: the physical hit opens the
	// invincibility window on its way through, and a second component arriving
	// behind it would be dropped by TryConsumeHit on any character authored with
	// one. Both halves come from a single TryConsumeHit, so both land.
	{
		const FVector TargetLocation(0.f, 200.f, 0.f);
		AARPGCombatDummy* Attacker = SpawnDummy(World, FVector(-400.f, 200.f, 0.f), TAG_Faction_Player, 100.f);
		AARPGCombatDummy* Target = SpawnArmouredTarget(TargetLocation, /*IFrames=*/5.f);

		UARPGHitboxComponent* Hitbox = AttachHitbox(Attacker, Physical, 100.f);
		Hitbox->SetSourceActor(Attacker);
		CoatWithFire(Hitbox, 40.f);

		Hitbox->ActivateHitbox();
		TickAt(Hitbox, FVector(-300.f, 200.f, 0.f));
		TickAt(Hitbox, TargetLocation);

		TestEqual(TEXT("i-frames opened by the physical hit do not swallow the elemental one"),
			GetHealth(Target), 500.f - 50.f - 40.f);
	}

	// --- A cleared rider leaves nothing behind ---------------------------------
	// The coating is per-window. One left on the hitbox would elementally charge
	// every later swing this character throws, for free and forever -- the same
	// trap the melee ability's damage-type snapshot exists to avoid.
	{
		const FVector TargetLocation(0.f, 400.f, 0.f);
		AARPGCombatDummy* Attacker = SpawnDummy(World, FVector(-400.f, 400.f, 0.f), TAG_Faction_Player, 100.f);
		AARPGCombatDummy* Target = SpawnArmouredTarget(TargetLocation, /*IFrames=*/0.f);

		UARPGHitboxComponent* Hitbox = AttachHitbox(Attacker, Physical, 100.f);
		Hitbox->SetSourceActor(Attacker);
		CoatWithFire(Hitbox, 40.f);

		Hitbox->ActivateHitbox();
		TickAt(Hitbox, FVector(-300.f, 400.f, 0.f));
		TickAt(Hitbox, TargetLocation);
		TestEqual(TEXT("coated swing lands both halves"), GetHealth(Target), 500.f - 50.f - 40.f);

		// The next swing is uncoated.
		Hitbox->ClearElementalRider();
		Hitbox->DeactivateHitbox();
		Hitbox->SetWorldLocation(FVector(-400.f, 400.f, 0.f));
		Hitbox->ActivateHitbox();
		TickAt(Hitbox, FVector(-300.f, 400.f, 0.f));
		TickAt(Hitbox, TargetLocation);

		TestEqual(TEXT("the following swing deals steel only"),
			GetHealth(Target), 500.f - 50.f - 40.f - 50.f);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
