// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetManager.h"
#include "GameplayTagContainer.h"
#include "ARPGAssetManager.generated.h"

class UARPGDamageTypeAsset;

/**
 * Custom asset manager. Two jobs.
 *
 * ONE: call UAbilitySystemGlobals::InitGlobalData() at the one point in startup
 * where it is safe to. This is not optional bookkeeping. Without it, GAS's
 * target-data scriptstruct cache is never built: ability activation prediction
 * misbehaves and anything sending FGameplayAbilityTargetData across the wire
 * crashes. It is the single most common GAS setup failure, and it fails late and
 * confusingly rather than at startup.
 *
 * TWO: be the tag-keyed index for content the C++ layer needs to name but must
 * not name by PATH.
 *
 * WHY THE INDEX EXISTS. Fifteen data-asset classes in this project override
 * GetPrimaryAssetId(), and for a long time not one primary asset type was
 * registered for scanning -- there was no PrimaryAssetTypesToScan entry
 * anywhere. The Asset Manager therefore never discovered any of them, so
 * GetPrimaryAssetIdList() returned nothing, async loading by type did not work,
 * and every one of those overrides was dead code. The gap got filled by hand:
 * ini enumerations in UARPGWorldSettings, and hardcoded FSoftObjectPaths in C++
 * -- including two inside ARPGCombat, a runtime library module that has no
 * business naming /Game/ content at all.
 *
 * The scan entries now live in DefaultGame.ini under
 * [/Script/Engine.AssetManagerSettings]. With them in place a damage type is
 * found by its OWN Damage.* tag, so dropping a new DA_Damage_* into the folder
 * is sufficient -- no config edit, no rebuild, and no path in C++.
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

	/** The primary asset type every UARPGDamageTypeAsset reports. */
	static const FPrimaryAssetType DamageTypeAssetType;

	/**
	 * The damage type carrying this Damage.* tag, or null if none is registered.
	 *
	 * STATIC AND FORGIVING, unlike Get(). A caller here is asking a question
	 * about content, and the honest answer in a harness that never installed our
	 * asset manager is "no damage types are registered" -- not a fatal. The
	 * damage execution already copes with a null damage type by falling back to
	 * the one on the effect context.
	 */
	static const UARPGDamageTypeAsset* FindDamageType(FGameplayTag DamageTypeTag);

	/**
	 * Loads every scanned damage type and indexes it by its own DamageTypeTag.
	 *
	 * SYNCHRONOUS, and deliberately so: there are seven of these, they are tiny,
	 * and the first thing that asks for one is a damage execution running inside
	 * a gameplay effect -- a context that cannot wait on a streaming handle. The
	 * cost is paid once per process.
	 */
	void BuildDamageTypeIndex();

protected:
	virtual void StartInitialLoading() override;

private:
	/** Damage.* tag -> asset. Built lazily; empty until BuildDamageTypeIndex runs. */
	UPROPERTY(Transient)
	TMap<FGameplayTag, TObjectPtr<const UARPGDamageTypeAsset>> DamageTypesByTag;

	bool bDamageTypeIndexBuilt = false;
};
