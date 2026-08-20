// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "ARPGTestFixtures.h"

#if WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS

#include "ARPGAssetManager.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGGameplayTags.h"
#include "Engine/AssetManager.h"

/**
 * The Asset Manager actually scans what fifteen classes declare.
 *
 * WHY THIS TEST EXISTS. Fifteen data-asset classes in this project override
 * GetPrimaryAssetId(), and for a long time no primary asset type was registered
 * for scanning at all -- there was no PrimaryAssetTypesToScan entry in any
 * config file. Nothing failed. GetPrimaryAssetIdList() simply returned an empty
 * array, async loading by type quietly did nothing, and all fifteen overrides
 * were dead code that looked live.
 *
 * That is the failure mode worth a test: it is invisible from the code, it is
 * one deleted ini section away from returning, and the symptom is not a crash
 * but content that silently stops being found.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGAssetScanTest,
	"ARPG.Core.AssetManager.PrimaryAssetTypesAreScanned",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGAssetScanTest::RunTest(const FString& Parameters)
{
	UAssetManager& Manager = UAssetManager::Get();

	// The seven shipped damage types live under /Game/ARPG/DamageTypes.
	TArray<FPrimaryAssetId> DamageTypes;
	Manager.GetPrimaryAssetIdList(UARPGAssetManager::DamageTypeAssetType, DamageTypes);

	TestTrue(TEXT("The asset manager scans ARPGDamageType at all"), DamageTypes.Num() > 0);
	if (DamageTypes.Num() == 0)
	{
		AddError(TEXT("No ARPGDamageType assets were scanned. Check the ")
			TEXT("+PrimaryAssetTypesToScan entries under ")
			TEXT("[/Script/Engine.AssetManagerSettings] in DefaultGame.ini."));
		return false;
	}

	// Every one of the other declared types resolves too. Listed by name rather
	// than derived, so that deleting a scan entry fails here rather than
	// silently narrowing what the game can find.
	const TCHAR* const DeclaredTypes[] = {
		TEXT("ARPGMagicElement"), TEXT("ARPGCombinationTable"), TEXT("ARPGMagicLoadout"),
		TEXT("ARPGAttack"), TEXT("ARPGAttackTree"), TEXT("ARPGWeapon"),
		TEXT("ARPGNPC"), TEXT("ARPGFluid"), TEXT("ARPGSolid")
	};

	for (const TCHAR* TypeName : DeclaredTypes)
	{
		TArray<FPrimaryAssetId> Found;
		Manager.GetPrimaryAssetIdList(FPrimaryAssetType(TypeName), Found);
		TestTrue(FString::Printf(TEXT("%s is scanned"), TypeName), Found.Num() > 0);
	}

	return true;
}

/**
 * A damage type is reachable by its OWN tag, with no path anywhere in C++.
 *
 * This is what replaced the two hardcoded /Game/ARPG/DamageTypes/... paths that
 * sat inside ARPGCombat -- a runtime library module that had no business naming
 * project content. Burning says "I deal Damage.Fire"; which asset answers for
 * fire is content's business.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGDamageTypeByTagTest,
	"ARPG.Core.AssetManager.DamageTypesResolveByTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGDamageTypeByTagTest::RunTest(const FString& Parameters)
{
	// The two the status effects depend on, plus physical as the control.
	const FGameplayTag Wanted[] = { TAG_Damage_Fire, TAG_Damage_Lightning, TAG_Damage_Physical };

	for (const FGameplayTag& Tag : Wanted)
	{
		const UARPGDamageTypeAsset* Asset = UARPGAssetManager::FindDamageType(Tag);

		if (!TestNotNull(*FString::Printf(TEXT("%s resolves to an asset"), *Tag.ToString()), Asset))
		{
			continue;
		}

		// The index is keyed by the asset's own tag, so a mismatch here would
		// mean the map was built from something other than DamageTypeTag.
		TestEqual(TEXT("and the asset agrees which tag it is"), Asset->DamageTypeTag, Tag);
	}

	// A tag nothing claims answers null rather than asserting -- the damage
	// execution relies on that to fall through to the effect context.
	const FGameplayTag Unclaimed = FGameplayTag::RequestGameplayTag(
		TEXT("Damage.Category.True"), /*ErrorIfNotFound=*/false);
	TestNull(TEXT("An unclaimed tag resolves to nothing rather than asserting"),
		UARPGAssetManager::FindDamageType(Unclaimed));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS
