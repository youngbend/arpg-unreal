// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/// Foundation layer: gameplay tags, base data assets, attribute sets, the
/// custom effect context, and the AbilitySystemGlobals/AssetManager overrides.
/// Depends on nothing in the ARPG chain — see Docs/UNREAL_PORT_PLAN.md §2.
public class ARPGCore : ModuleRules
{
	public ARPGCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
