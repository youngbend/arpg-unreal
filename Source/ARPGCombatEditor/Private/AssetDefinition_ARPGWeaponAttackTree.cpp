// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetDefinition_ARPGWeaponAttackTree.h"
#include "ARPGAttackTreeEditorToolkit.h"
#include "ARPGWeaponAttackTree.h"

#define LOCTEXT_NAMESPACE "ARPGCombatEditor"

FText UAssetDefinition_ARPGWeaponAttackTree::GetAssetDisplayName() const
{
	return LOCTEXT("AttackTreeAssetName", "Weapon Attack Tree");
}

FLinearColor UAssetDefinition_ARPGWeaponAttackTree::GetAssetColor() const
{
	return FLinearColor(FColor(160, 60, 60));
}

TSoftClassPtr<UObject> UAssetDefinition_ARPGWeaponAttackTree::GetAssetClass() const
{
	return UARPGWeaponAttackTree::StaticClass();
}

TConstArrayView<FAssetCategoryPath> UAssetDefinition_ARPGWeaponAttackTree::GetAssetCategories() const
{
	static const auto Categories = { EAssetCategoryPaths::Gameplay };
	return Categories;
}

EAssetCommandResult UAssetDefinition_ARPGWeaponAttackTree::OpenAssets(const FAssetOpenArgs& OpenArgs) const
{
	for (UARPGWeaponAttackTree* Tree : OpenArgs.LoadObjects<UARPGWeaponAttackTree>())
	{
		const TSharedRef<FARPGAttackTreeEditor> Editor = MakeShared<FARPGAttackTreeEditor>();
		Editor->InitEditor(OpenArgs.GetToolkitMode(), OpenArgs.ToolkitHost, Tree);
	}

	return EAssetCommandResult::Handled;
}

#undef LOCTEXT_NAMESPACE
