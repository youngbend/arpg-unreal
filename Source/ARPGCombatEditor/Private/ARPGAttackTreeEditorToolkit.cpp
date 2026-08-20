// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGAttackTreeEditorToolkit.h"
#include "ARPGAttackTreeGraph.h"
#include "ARPGAttackTreeGraphNode.h"
#include "ARPGAttackTreeGraphSchema.h"
#include "ARPGWeaponAttackTree.h"
#include "EdGraph/EdGraph.h"
#include "Framework/Commands/GenericCommands.h"
#include "GraphEditor.h"
#include "IDetailsView.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "ARPGAttackTreeEditor"

const FName FARPGAttackTreeEditor::GraphTabId(TEXT("ARPGAttackTreeEditor_Graph"));
const FName FARPGAttackTreeEditor::DetailsTabId(TEXT("ARPGAttackTreeEditor_Details"));
const FName FARPGAttackTreeEditor::AppIdentifier(TEXT("ARPGAttackTreeEditorApp"));

void FARPGAttackTreeEditor::InitEditor(const EToolkitMode::Type Mode,
	const TSharedPtr<IToolkitHost>& InitToolkitHost, UARPGWeaponAttackTree* InAttackTree)
{
	AttackTree = InAttackTree;

	FPropertyEditorModule& PropertyModule =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	FDetailsViewArgs DetailsArgs;
	DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	DetailsArgs.bHideSelectionTip = true;
	DetailsArgs.NotifyHook = this;
	DetailsView = PropertyModule.CreateDetailView(DetailsArgs);
	DetailsView->SetObject(InAttackTree);

	BindGraphCommands();

	const TSharedRef<FTabManager::FLayout> Layout =
		FTabManager::NewLayout("Standalone_ARPGAttackTreeEditor_v1")
		->AddArea(
			FTabManager::NewPrimaryArea()->SetOrientation(Orient_Vertical)
			->Split(
				FTabManager::NewSplitter()->SetOrientation(Orient_Horizontal)
				->Split(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.75f)
					->AddTab(GraphTabId, ETabState::OpenedTab)
				)
				->Split(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.25f)
					->AddTab(DetailsTabId, ETabState::OpenedTab)
				)
			)
		);

	InitAssetEditor(Mode, InitToolkitHost, AppIdentifier, Layout,
		/*bCreateDefaultStandaloneMenu=*/true, /*bCreateDefaultToolbar=*/true, InAttackTree);
}

void FARPGAttackTreeEditor::BindGraphCommands()
{
	GraphCommands = MakeShared<FUICommandList>();

	// Bound through lambdas rather than by member-function pointer: the
	// implementations live on FAIGraphEditor, and a delegate bound to a base
	// class's method with a derived object is a template deduction argument
	// nobody should have to have.
	GraphCommands->MapAction(FGenericCommands::Get().SelectAll,
		FExecuteAction::CreateLambda([this] { SelectAllNodes(); }),
		FCanExecuteAction::CreateLambda([this] { return CanSelectAllNodes(); }));

	GraphCommands->MapAction(FGenericCommands::Get().Delete,
		FExecuteAction::CreateLambda([this] { DeleteSelectedNodes(); }),
		FCanExecuteAction::CreateLambda([this] { return CanDeleteNodes(); }));

	GraphCommands->MapAction(FGenericCommands::Get().Copy,
		FExecuteAction::CreateLambda([this] { CopySelectedNodes(); }),
		FCanExecuteAction::CreateLambda([this] { return CanCopyNodes(); }));

	GraphCommands->MapAction(FGenericCommands::Get().Cut,
		FExecuteAction::CreateLambda([this] { CutSelectedNodes(); }),
		FCanExecuteAction::CreateLambda([this] { return CanCutNodes(); }));

	GraphCommands->MapAction(FGenericCommands::Get().Paste,
		FExecuteAction::CreateLambda([this] { PasteNodes(); }),
		FCanExecuteAction::CreateLambda([this] { return CanPasteNodes(); }));

	GraphCommands->MapAction(FGenericCommands::Get().Duplicate,
		FExecuteAction::CreateLambda([this] { DuplicateNodes(); }),
		FCanExecuteAction::CreateLambda([this] { return CanDuplicateNodes(); }));
}

UEdGraph* FARPGAttackTreeEditor::GetOrCreateGraph()
{
	UARPGWeaponAttackTree* Tree = AttackTree.Get();
	if (!Tree)
	{
		return nullptr;
	}

	if (!Tree->EdGraph)
	{
		Tree->EdGraph = FBlueprintEditorUtils::CreateNewGraph(Tree, TEXT("AttackTree"),
			UARPGAttackTreeGraph::StaticClass(), UARPGAttackTreeGraphSchema::StaticClass());
		Tree->EdGraph->bAllowDeletion = false;

		// Every tree that exists today reaches this branch: the sword tree was
		// built by a Python script, and nothing before now wrote a graph. Laying
		// it out from the arrays is what stops the first open from presenting an
		// empty canvas over a finished moveset -- and the first save from
		// compiling that emptiness back over it.
		CastChecked<UARPGAttackTreeGraph>(Tree->EdGraph)->RebuildFromAsset();

		// The graph is new state worth keeping, so the asset is genuinely dirty
		// after this -- opening it changed it.
		Tree->MarkPackageDirty();
	}

	return Tree->EdGraph;
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

void FARPGAttackTreeEditor::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(
		LOCTEXT("WorkspaceMenu", "Attack Tree Editor"));

	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	InTabManager->RegisterTabSpawner(GraphTabId,
		FOnSpawnTab::CreateSP(this, &FARPGAttackTreeEditor::SpawnGraphTab))
		.SetDisplayName(LOCTEXT("GraphTab", "Combo Graph"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef());

	InTabManager->RegisterTabSpawner(DetailsTabId,
		FOnSpawnTab::CreateSP(this, &FARPGAttackTreeEditor::SpawnDetailsTab))
		.SetDisplayName(LOCTEXT("DetailsTab", "Details"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef());
}

void FARPGAttackTreeEditor::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);
	InTabManager->UnregisterTabSpawner(GraphTabId);
	InTabManager->UnregisterTabSpawner(DetailsTabId);
}

TSharedRef<SDockTab> FARPGAttackTreeEditor::SpawnGraphTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("GraphTabTitle", "Combo Graph"))
		[
			CreateGraphEditorWidget()
		];
}

TSharedRef<SDockTab> FARPGAttackTreeEditor::SpawnDetailsTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("DetailsTabTitle", "Details"))
		[
			DetailsView.ToSharedRef()
		];
}

TSharedRef<SGraphEditor> FARPGAttackTreeEditor::CreateGraphEditorWidget()
{
	SGraphEditor::FGraphEditorEvents Events;
	Events.OnSelectionChanged = SGraphEditor::FOnSelectionChanged::CreateSP(
		this, &FARPGAttackTreeEditor::OnSelectedNodesChanged);

	FGraphAppearanceInfo Appearance;
	Appearance.CornerText = LOCTEXT("GraphCorner", "COMBO");

	GraphEditorWidget = SNew(SGraphEditor)
		.AdditionalCommands(GraphCommands)
		.IsEditable(true)
		.Appearance(Appearance)
		.GraphToEdit(GetOrCreateGraph())
		.GraphEvents(Events);

	// What FAIGraphEditor's delete/copy/paste act on. Without this they have no
	// widget to read a selection from and every command silently does nothing.
	UpdateGraphEdPtr = GraphEditorWidget;

	return GraphEditorWidget.ToSharedRef();
}

// ---------------------------------------------------------------------------

void FARPGAttackTreeEditor::OnSelectedNodesChanged(const TSet<UObject*>& NewSelection)
{
	if (!DetailsView.IsValid())
	{
		return;
	}

	// THE RUNTIME NODE IS WHAT IS SHOWN, not the graph node wrapping it: the
	// attack, the reset override and everything else worth editing lives there,
	// and a details panel showing the box instead would list pin arrays.
	TArray<UObject*> Selected;
	for (UObject* Object : NewSelection)
	{
		if (const UARPGAttackTreeGraphNode_Attack* Node = Cast<UARPGAttackTreeGraphNode_Attack>(Object))
		{
			if (Node->ComboNode)
			{
				Selected.Add(Node->ComboNode);
			}
		}
	}

	if (Selected.Num() > 0)
	{
		DetailsView->SetObjects(Selected);
	}
	else
	{
		// Nothing selected falls back to the asset itself, which is where the
		// reset timeout, the air attacks and the block definition live -- so
		// clicking empty canvas is how you reach them rather than a dead panel.
		DetailsView->SetObject(AttackTree.Get());
	}
}

void FARPGAttackTreeEditor::NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent,
	FProperty* PropertyThatChanged)
{
	// Assigning an attack renames the box and repaints it -- the title and the
	// colour both read from the runtime node, which the details panel just
	// wrote to.
	if (GraphEditorWidget.IsValid())
	{
		GraphEditorWidget->NotifyGraphChanged();
	}

	if (UARPGWeaponAttackTree* Tree = AttackTree.Get())
	{
		Tree->MarkPackageDirty();
	}
}

FName FARPGAttackTreeEditor::GetToolkitFName() const
{
	return FName("ARPGAttackTreeEditor");
}

FText FARPGAttackTreeEditor::GetBaseToolkitName() const
{
	return LOCTEXT("ToolkitName", "Attack Tree Editor");
}

FString FARPGAttackTreeEditor::GetWorldCentricTabPrefix() const
{
	return LOCTEXT("TabPrefix", "AttackTree ").ToString();
}

FLinearColor FARPGAttackTreeEditor::GetWorldCentricTabColorScale() const
{
	return FLinearColor(0.1f, 0.3f, 0.5f, 0.5f);
}

#undef LOCTEXT_NAMESPACE
