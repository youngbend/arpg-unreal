// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/// Editor-only tooling for the combat layer: the attack tree node graph.
///
/// A SEPARATE MODULE, not a WITH_EDITOR block inside ARPGCombat, because
/// everything here links UnrealEd and GraphEditor -- modules a shipping build
/// does not have. A runtime module that references them fails to link the moment
/// anyone packages the game, which is the usual way this mistake is found.
public class ARPGCombatEditor : ModuleRules
{
	public ARPGCombatEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"ARPGCombat"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			// The base classes the Behavior Tree and EQS editors are built on:
			// UAIGraph, UAIGraphNode and FAIGraphEditor. Taking these gets node
			// selection, delete, cut/copy/paste and duplicate as inherited
			// behaviour rather than another 200 lines of clipboard plumbing.
			"AIGraph",

			// UAssetDefinitionDefault, which replaced FAssetTypeActions_Base as
			// the way an asset type declares how it opens.
			"AssetDefinition",

			"ApplicationCore",
			"EditorFramework",
			"GraphEditor",
			"InputCore",
			"PropertyEditor",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"UnrealEd"
		});
	}
}
