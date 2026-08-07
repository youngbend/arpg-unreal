// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class arpg : ModuleRules
{
	public arpg(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate",
			"SlateCore",

			// Gameplay Ability System.
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",

			// ARPG runtime modules. The game module sits on top of the whole
			// chain; see Docs/UNREAL_PORT_PLAN.md section 2 for the DAG.
			"ARPGCore",
			"ARPGCombat",
			"ARPGMagic",
			"ARPGWorld",
			"ARPGAI"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		PublicIncludePaths.AddRange(new string[] {
			"arpg"
		});
	}
}
