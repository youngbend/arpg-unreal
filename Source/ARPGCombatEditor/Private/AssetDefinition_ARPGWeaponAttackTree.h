// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetDefinitionDefault.h"
#include "AssetDefinition_ARPGWeaponAttackTree.generated.h"

/**
 * Makes double-clicking a weapon attack tree open the graph rather than a
 * property list.
 *
 * A UAssetDefinition, which replaced FAssetTypeActions_Base: the engine finds it
 * by scanning for the class, so unlike the old registration there is nothing to
 * remember to do in a module's startup -- and nothing to forget to undo in its
 * shutdown.
 */
UCLASS()
class UAssetDefinition_ARPGWeaponAttackTree : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	virtual FText GetAssetDisplayName() const override;
	virtual FLinearColor GetAssetColor() const override;
	virtual TSoftClassPtr<UObject> GetAssetClass() const override;
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override;
	virtual EAssetCommandResult OpenAssets(const FAssetOpenArgs& OpenArgs) const override;
};
