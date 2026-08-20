// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "ARPGTestFixtures.h"

#if WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS

#include "ARPGArmorComponent.h"
#include "ARPGCloakComponent.h"
#include "ARPGCombatDummy.h"
#include "ARPGCombatProgressionTrackers.h"
#include "ARPGComboComponent.h"
#include "ARPGHitStopComponent.h"
#include "ARPGHurtboxComponent.h"
#include "ARPGInventoryComponent.h"
#include "ARPGLocomotionComponent.h"
#include "ARPGMagicComponent.h"
#include "ARPGMagicProgressionTracker.h"
#include "ARPGModalInputComponent.h"
#include "ARPGNPCCharacter.h"
#include "ARPGNoiseComponent.h"
#include "ARPGParryComponent.h"
#include "ARPGPlayerActionComponent.h"
#include "ARPGPoiseComponent.h"
#include "ARPGProgressionComponent.h"
#include "ARPGQuickSlotComponent.h"
#include "ARPGStatusResistanceComponent.h"
#include "ARPGVitalRegenComponent.h"
#include "ARPGWeaponComponent.h"
#include "arpgCharacter.h"

/**
 * The actors the game actually plays as carry the components they need.
 *
 * WHY THIS TEST EXISTS, and it is the whole point of it: ten components were
 * implemented, documented and covered by passing automation tests, and attached
 * to nothing. Every existing test built its own bare actor and added the
 * component it wanted by hand, so the suite went green on components the player
 * character had never owned. UARPGHurtboxComponent was among them, which meant
 * the player was invulnerable to every hitbox in the game -- silently, because
 * UARPGHitboxComponent skips an actor whose hurtbox does not resolve rather than
 * complaining about it.
 *
 * No test could have caught that, because no test looked at the real pawn. This
 * one does, and it asserts against the CDO rather than a spawned actor: default
 * subobjects exist on the class default object, so the check needs no world, no
 * mesh and no possession, and it fails at the point the constructor stops
 * creating something.
 */
namespace ARPGCompositionTestUtils
{
	/** Reports which of the wanted component classes the CDO does not have. */
	template <typename ActorType>
	TArray<FString> FindMissing(const TArray<UClass*>& Wanted)
	{
		// Mutable only because GetDefaultSubobjects is non-const; nothing here
		// writes to the CDO.
		ActorType* CDO = GetMutableDefault<ActorType>();
		check(CDO);

		TArray<UObject*> Subobjects;
		CDO->GetDefaultSubobjects(Subobjects);

		TArray<FString> Missing;
		for (UClass* Class : Wanted)
		{
			const bool bFound = Subobjects.ContainsByPredicate(
				[Class](const UObject* Object) { return Object && Object->IsA(Class); });

			if (!bFound)
			{
				Missing.Add(Class->GetName());
			}
		}
		return Missing;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPlayerCompositionTest,
	"ARPG.Composition.PlayerCharacterCarriesItsComponents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPlayerCompositionTest::RunTest(const FString& Parameters)
{
	const TArray<UClass*> Required = {
		// Was already present.
		UARPGComboComponent::StaticClass(),
		UARPGModalInputComponent::StaticClass(),
		UARPGLocomotionComponent::StaticClass(),
		UARPGVitalRegenComponent::StaticClass(),
		UARPGPlayerActionComponent::StaticClass(),
		UARPGWeaponComponent::StaticClass(),
		UARPGParryComponent::StaticClass(),
		UARPGMagicComponent::StaticClass(),
		UARPGInventoryComponent::StaticClass(),
		UARPGQuickSlotComponent::StaticClass(),
		UARPGNoiseComponent::StaticClass(),

		// Was NOT. Each of these was implemented and tested while attached to
		// nothing the player ever played as.
		UARPGHurtboxComponent::StaticClass(),          // or the player takes no damage at all
		UARPGPoiseComponent::StaticClass(),
		UARPGArmorComponent::StaticClass(),
		UARPGHitStopComponent::StaticClass(),
		UARPGStatusResistanceComponent::StaticClass(),
		UARPGCloakComponent::StaticClass(),            // or RT+B does nothing
		UARPGProgressionComponent::StaticClass(),
		UARPGWeaponProgressionTracker::StaticClass(),
		UARPGArmorProgressionTracker::StaticClass(),
		UARPGMagicProgressionTracker::StaticClass()
	};

	const TArray<FString> Missing =
		ARPGCompositionTestUtils::FindMissing<AarpgCharacter>(Required);

	TestTrue(FString::Printf(TEXT("AarpgCharacter is missing: %s"), *FString::Join(Missing, TEXT(", "))),
		Missing.IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGNPCCompositionTest,
	"ARPG.Composition.NPCCharacterCarriesItsComponents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGNPCCompositionTest::RunTest(const FString& Parameters)
{
	const TArray<UClass*> Required = {
		UARPGHurtboxComponent::StaticClass(),
		UARPGPoiseComponent::StaticClass(),
		UARPGComboComponent::StaticClass(),
		UARPGWeaponComponent::StaticClass(),
		UARPGParryComponent::StaticClass(),
		UARPGInventoryComponent::StaticClass(),
		UARPGQuickSlotComponent::StaticClass(),
		UARPGNoiseComponent::StaticClass(),

		// Hit-stop needs a component at BOTH ends of an exchange, so an NPC
		// without one silently cancels the freeze on the player's hits too.
		UARPGHitStopComponent::StaticClass(),
		UARPGStatusResistanceComponent::StaticClass()
	};

	const TArray<FString> Missing =
		ARPGCompositionTestUtils::FindMissing<AARPGNPCCharacter>(Required);

	TestTrue(FString::Printf(TEXT("AARPGNPCCharacter is missing: %s"), *FString::Join(Missing, TEXT(", "))),
		Missing.IsEmpty());

	// DELIBERATELY ABSENT, asserted so nobody "fixes" it by symmetry with the
	// player: UARPGNPCDefinition has no armour field, so nothing would ever
	// equip any. An NPC's mitigation comes from its archetype's attributes.
	const TArray<FString> ShouldBeMissing =
		ARPGCompositionTestUtils::FindMissing<AARPGNPCCharacter>({ UARPGArmorComponent::StaticClass() });

	TestEqual(TEXT("An NPC has no armour component, because nothing equips armour on one"),
		ShouldBeMissing.Num(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGCombatDummyCompositionTest,
	"ARPG.Composition.CombatDummyIsDamageable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGCombatDummyCompositionTest::RunTest(const FString& Parameters)
{
	// The dummy exists to be hit, so the hurtbox is the whole of its contract.
	const TArray<FString> Missing = ARPGCompositionTestUtils::FindMissing<AARPGCombatDummy>(
		{ UARPGHurtboxComponent::StaticClass() });

	TestTrue(FString::Printf(TEXT("AARPGCombatDummy is missing: %s"), *FString::Join(Missing, TEXT(", "))),
		Missing.IsEmpty());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS
