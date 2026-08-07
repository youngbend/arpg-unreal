// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetManager.h"
#include "ARPGAssetManager.generated.h"

/**
 * Custom asset manager, whose job right now is to call
 * UAbilitySystemGlobals::InitGlobalData() at the one point in startup where it
 * is safe to.
 *
 * This is not optional bookkeeping. Without InitGlobalData(), GAS's target-data
 * scriptstruct cache is never built: ability activation prediction misbehaves
 * and anything sending FGameplayAbilityTargetData across the wire crashes. It
 * is the single most common GAS setup failure, and it fails late and
 * confusingly rather than at startup.
 *
 * Must be registered in DefaultEngine.ini:
 *
 *   [/Script/Engine.Engine]
 *   AssetManagerClassName="/Script/ARPGCore.ARPGAssetManager"
 */
UCLASS()
class ARPGCORE_API UARPGAssetManager : public UAssetManager
{
	GENERATED_BODY()

public:
	/** Returns the configured asset manager. Fatals if the ini wiring is wrong. */
	static UARPGAssetManager& Get();

protected:
	virtual void StartInitialLoading() override;
};
