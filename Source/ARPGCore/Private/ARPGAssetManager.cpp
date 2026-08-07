// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGAssetManager.h"
#include "ARPGCore.h"
#include "AbilitySystemGlobals.h"
#include "Engine/Engine.h"

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
