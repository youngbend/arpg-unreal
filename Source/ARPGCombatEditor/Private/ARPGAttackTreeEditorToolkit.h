// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AIGraphEditor.h"
#include "Misc/NotifyHook.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/WeakObjectPtr.h"

class IDetailsView;
class SGraphEditor;
class UARPGWeaponAttackTree;
class UEdGraph;

/**
 * The window a weapon's moveset is authored in: a canvas and a details panel.
 *
 * INHERITS FAIGraphEditor for the parts of a graph editor that are the same in
 * every graph editor -- select all, delete, cut, copy, paste, duplicate, and the
 * undo hookup underneath them. Those are the members relied on here:
 *
 *     UpdateGraphEdPtr                    the widget the commands act on
 *     SelectAllNodes / CanSelectAllNodes
 *     DeleteSelectedNodes / CanDeleteNodes
 *     CutSelectedNodes / CanCutNodes
 *     CopySelectedNodes / CanCopyNodes
 *     PasteNodes / CanPasteNodes
 *     DuplicateNodes / CanDuplicateNodes
 *     OnSelectedNodesChanged
 *
 * Listed because they are the whole of this class's dependency on that base: if
 * a future engine version renames one, the breakage is here and nowhere else.
 */
class FARPGAttackTreeEditor : public FAssetEditorToolkit, public FAIGraphEditor, public FNotifyHook
{
public:
	static const FName GraphTabId;
	static const FName DetailsTabId;
	static const FName AppIdentifier;

	void InitEditor(const EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost,
		UARPGWeaponAttackTree* InAttackTree);

	//~ IToolkit
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;
	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;

	//~ FAIGraphEditor
	virtual void OnSelectedNodesChanged(const TSet<UObject*>& NewSelection) override;

	//~ FNotifyHook
	virtual void NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent,
		FProperty* PropertyThatChanged) override;

private:
	TSharedRef<SDockTab> SpawnGraphTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnDetailsTab(const FSpawnTabArgs& Args);
	TSharedRef<SGraphEditor> CreateGraphEditorWidget();
	void BindGraphCommands();

	/**
	 * The canvas for this asset, built from its arrays the first time it is
	 * opened. See UARPGAttackTreeGraph::RebuildFromAsset.
	 */
	UEdGraph* GetOrCreateGraph();

	/**
	 * Weak on purpose: the asset editor subsystem holds the asset for as long as
	 * this toolkit is open, so an owning pointer here would only be a second
	 * claim on the same thing -- and a stale one during teardown.
	 */
	TWeakObjectPtr<UARPGWeaponAttackTree> AttackTree;

	TSharedPtr<IDetailsView> DetailsView;
	TSharedPtr<SGraphEditor> GraphEditorWidget;
	TSharedPtr<FUICommandList> GraphCommands;
};
