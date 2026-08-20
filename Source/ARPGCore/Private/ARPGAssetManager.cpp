// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGAssetManager.h"
#include "ARPGCore.h"
#include "ARPGDamageTypeAsset.h"
#include "AbilitySystemGlobals.h"
#include "Engine/Engine.h"

const FPrimaryAssetType UARPGAssetManager::DamageTypeAssetType = TEXT("ARPGDamageType");

UARPGAssetManager& UARPGAssetManager::Get()
{
	check(GEngine);

	if (UARPGAssetManager* Singleton = Cast<UARPGAssetManager>(GEngine->AssetManager))
	{
		return *Singleton;
	}

	UE_LOG(LogARPGCore, Fatal,
		TEXT("Expected UARPGAssetManager but found %s. Set AssetManagerClassName in ")
		TEXT("DefaultEngine.ini to \"/Script/ARPGCore.ARPGAssetManager\"."),
		*GetNameSafe(GEngine->AssetManager));

	// Unreachable -- UE_LOG(Fatal) does not return. Present so every path
	// returns a reference.
	return *NewObject<UARPGAssetManager>();
}

const UARPGDamageTypeAsset* UARPGAssetManager::FindDamageType(FGameplayTag DamageTypeTag)
{
	if (!DamageTypeTag.IsValid() || !GEngine)
	{
		return nullptr;
	}

	// Deliberately not Get(): see the header. An automation harness or a
	// commandlet running the stock UAssetManager should get "nothing registered"
	// rather than a fatal.
	UARPGAssetManager* Manager = Cast<UARPGAssetManager>(GEngine->AssetManager);
	if (!Manager)
	{
		return nullptr;
	}

	if (!Manager->bDamageTypeIndexBuilt)
	{
		Manager->BuildDamageTypeIndex();
	}

	const TObjectPtr<const UARPGDamageTypeAsset>* Found = Manager->DamageTypesByTag.Find(DamageTypeTag);
	return Found ? Found->Get() : nullptr;
}

void UARPGAssetManager::BuildDamageTypeIndex()
{
	// Set FIRST, not last. A damage type asset whose OnDamageApplied happens to
	// ask for another damage type would otherwise recurse into this function
	// forever; a half-built index that answers null is the recoverable failure.
	bDamageTypeIndexBuilt = true;
	DamageTypesByTag.Reset();

	TArray<FPrimaryAssetId> Ids;
	GetPrimaryAssetIdList(DamageTypeAssetType, Ids);

	if (Ids.IsEmpty())
	{
		// Not fatal, but almost always a wiring mistake rather than a project
		// with no damage types -- so it says which knob is wrong.
		UE_LOG(LogARPGCore, Warning,
			TEXT("No %s assets scanned. Check +PrimaryAssetTypesToScan in DefaultGame.ini ")
			TEXT("under [/Script/Engine.AssetManagerSettings]."),
			*DamageTypeAssetType.ToString());
		return;
	}

	for (const FPrimaryAssetId& Id : Ids)
	{
		const FSoftObjectPath Path = GetPrimaryAssetPath(Id);
		const UARPGDamageTypeAsset* Asset = Cast<UARPGDamageTypeAsset>(Path.TryLoad());
		if (!Asset)
		{
			continue;
		}

		if (!Asset->DamageTypeTag.IsValid())
		{
			UE_LOG(LogARPGCore, Warning,
				TEXT("%s has no DamageTypeTag and cannot be found by tag."), *Id.ToString());
			continue;
		}

		// First writer wins, and the collision is logged rather than silently
		// resolved: two assets claiming Damage.Fire is an authoring error, and
		// which one a given hit resolves to would otherwise depend on scan order.
		if (const TObjectPtr<const UARPGDamageTypeAsset>* Existing = DamageTypesByTag.Find(Asset->DamageTypeTag))
		{
			UE_LOG(LogARPGCore, Warning,
				TEXT("Damage type tag %s is claimed by both %s and %s; keeping the first."),
				*Asset->DamageTypeTag.ToString(), *GetNameSafe(Existing->Get()), *GetNameSafe(Asset));
			continue;
		}

		DamageTypesByTag.Add(Asset->DamageTypeTag, Asset);
	}

	UE_LOG(LogARPGCore, Log, TEXT("Indexed %d damage types by tag."), DamageTypesByTag.Num());
}

void UARPGAssetManager::StartInitialLoading()
{
	Super::StartInitialLoading();

	// See the header: this is the safe point in startup for it, and skipping it
	// breaks ability prediction and target-data replication.
	UAbilitySystemGlobals::Get().InitGlobalData();

	// Deliberately logged rather than silent. A wrong AssetManagerClassName in
	// DefaultEngine.ini does not error -- the engine just constructs the base
	// UAssetManager, this override never runs, and GAS breaks much later
	// somewhere unrelated. Seeing this line is the cheap proof it is wired up.
	UE_LOG(LogARPGCore, Log,
		TEXT("UARPGAssetManager::StartInitialLoading -- AbilitySystemGlobals init complete (globals class: %s)"),
		*GetNameSafe(UAbilitySystemGlobals::Get().GetClass()));
}
