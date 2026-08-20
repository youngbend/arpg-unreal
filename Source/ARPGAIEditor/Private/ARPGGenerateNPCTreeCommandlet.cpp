// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGGenerateNPCTreeCommandlet.h"
#include "ARPGAIEditorModule.h"
#include "ARPGNPCDefinition.h"
#include "ARPGStateTreeConditions.h"
#include "ARPGStateTreeTasks.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/Factory.h"
#include "FileHelpers.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeFactory.h"
#include "StateTreeState.h"
#include "Tasks/AITask_MoveTo.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Components/StateTreeAIComponentSchema.h"

namespace
{
	const TCHAR* TreePackagePath = TEXT("/Game/ARPG/NPCs/ST_NPC_Melee");
	const TCHAR* TreeAssetName   = TEXT("ST_NPC_Melee");
	const TCHAR* NPCDefinitionPath = TEXT("/Game/ARPG/NPCs/DA_NPC_TestDummy.DA_NPC_TestDummy");

	/** Saves one package, and treats a refusal as an error rather than a no-op. */
	bool SavePackageFor(UObject* Asset)
	{
		if (!Asset)
		{
			return false;
		}

		UPackage* Package = Asset->GetOutermost();
		Package->SetDirtyFlag(true);

		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;

		if (UPackage::SavePackage(Package, nullptr, *FileName, Args))
		{
			return true;
		}

		UE_LOG(LogARPGAIEditor, Error, TEXT("Failed to save '%s'."), *Package->GetName());
		return false;
	}

	/**
	 * The tree asset, created with the AI schema if it does not exist yet.
	 *
	 * SetSchemaClass BEFORE FactoryCreateNew, and this is the whole reason the
	 * commandlet exists: with no schema set, UStateTreeFactory::ConfigureProperties
	 * opens a modal class picker, which under -unattended returns nothing and the
	 * factory hands back null.
	 */
	UStateTree* LoadOrCreateTree()
	{
		if (UStateTree* Existing = LoadObject<UStateTree>(nullptr,
			*FString::Printf(TEXT("%s.%s"), TreePackagePath, TreeAssetName)))
		{
			UE_LOG(LogARPGAIEditor, Display, TEXT("Rebuilding existing %s."), TreeAssetName);
			return Existing;
		}

		UPackage* Package = CreatePackage(TreePackagePath);
		if (!Package)
		{
			UE_LOG(LogARPGAIEditor, Error, TEXT("Could not create package %s."), TreePackagePath);
			return nullptr;
		}

		UStateTreeFactory* Factory = NewObject<UStateTreeFactory>();
		Factory->SetSchemaClass(UStateTreeAIComponentSchema::StaticClass());

		UStateTree* Tree = Cast<UStateTree>(Factory->FactoryCreateNew(
			UStateTree::StaticClass(), Package, FName(TreeAssetName),
			RF_Public | RF_Standalone, nullptr, GWarn));

		if (!Tree)
		{
			UE_LOG(LogARPGAIEditor, Error,
				TEXT("The factory returned nothing. That normally means the schema was "
					 "not set and it fell back to a modal picker."));
			return nullptr;
		}

		FAssetRegistryModule::AssetCreated(Tree);
		UE_LOG(LogARPGAIEditor, Display, TEXT("Created %s."), TreeAssetName);
		return Tree;
	}
}

UARPGGenerateNPCTreeCommandlet::UARPGGenerateNPCTreeCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UARPGGenerateNPCTreeCommandlet::Main(const FString& Params)
{
	UStateTree* Tree = LoadOrCreateTree();
	if (!Tree)
	{
		return 1;
	}

	UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(Tree->EditorData);
	if (!EditorData)
	{
		UE_LOG(LogARPGAIEditor, Error, TEXT("%s has no editor data."), TreeAssetName);
		return 1;
	}

	// REBUILT FROM SCRATCH rather than appended to, so a second run does not
	// produce a second Fight state.
	EditorData->SubTrees.Reset();
	EditorData->Schema = NewObject<UStateTreeAIComponentSchema>(EditorData, NAME_None, RF_Transactional);

	UStateTreeState& Root = EditorData->AddRootState();

	// --- Recover ------------------------------------------------------------
	//
	// ABOVE Fight, because selection is top-down first-match and this is an
	// answer to "stop fighting". Checked after the combat branch it would only
	// ever be acted on once the current attack finished.
	{
		UStateTreeState& Recover = Root.AddChildState(FName(TEXT("Recover")));

		// AddEnterCondition returns the NODE WRAPPER, not the struct -- GetNode()
		// reaches the FInstancedStruct's contents inside it.
		Recover.AddEnterCondition<FARPGStateTreeCondition_HealthBelow>()
			.GetNode().Threshold = 0.35f;

		// -1 means "whatever is selected". An NPC has no thumbs, but the test
		// archetype only carries one consumable, so the default is right.
		Recover.AddTask<FARPGStateTreeTask_UseConsumable>()
			.GetNode().SlotIndex = -1;
	}

	// --- Fight --------------------------------------------------------------
	//
	// No enter condition: this is the fallback, and a placeholder NPC that
	// swings unconditionally is honest about being one. The real tree gates
	// this on the perception evaluator's bHasTarget -- see the header for why
	// that is not here.
	{
		UStateTreeState& Fight = Root.AddChildState(FName(TEXT("Fight")));

		Fight.AddTask<FARPGStateTreeTask_Attack>()
			.GetNode().Input = EARPGAttackInput::Light;
	}

	// --- Compile ------------------------------------------------------------
	//
	// NOT OPTIONAL. The runtime reads a baked representation, not the editor
	// data above, so an uncompiled tree loads fine and does nothing at all.
	FStateTreeCompilerLog Log;
	if (!UStateTreeEditingSubsystem::CompileStateTree(Tree, Log))
	{
		UE_LOG(LogARPGAIEditor, Error, TEXT("%s failed to compile:"), TreeAssetName);
		Log.DumpToLog(LogARPGAIEditor);
		return 1;
	}

	if (!SavePackageFor(Tree))
	{
		return 1;
	}

	UE_LOG(LogARPGAIEditor, Display, TEXT("Compiled and saved %s (%d states under Root)."),
		TreeAssetName, Root.Children.Num());

	// --- Point the archetype at it ------------------------------------------
	//
	// The tree is inert until something runs it, and UARPGNPCComponent reads
	// this one field. Leaving it unset is how the asset came to exist with
	// nothing referencing it in the first place.
	UARPGNPCDefinition* Definition = LoadObject<UARPGNPCDefinition>(nullptr, NPCDefinitionPath);
	if (!Definition)
	{
		UE_LOG(LogARPGAIEditor, Warning,
			TEXT("No %s -- the tree is built but nothing runs it. Run "
				 "Tools/generate_test_npc.py first."), NPCDefinitionPath);
		return 0;
	}

	Definition->StateTreeRef.SetStateTree(Tree);
	Definition->StateTreeRef.SyncParameters();

	if (!SavePackageFor(Definition))
	{
		return 1;
	}

	UE_LOG(LogARPGAIEditor, Display,
		TEXT("DA_NPC_TestDummy now runs %s."), TreeAssetName);

	return 0;
}
