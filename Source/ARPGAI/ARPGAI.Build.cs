// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/// NPC layer: perception, behavior tree tasks/decorators, NPC definitions.
///
/// Sits beside ARPGMagic in the DAG rather than above it — the AI reads combat
/// state (poise, attack timing, health) but never magic internals.
public class ARPGAI : ModuleRules
{
	public ARPGAI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"AIModule",
			"NavigationSystem",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			"ARPGCore",
			"ARPGCombat"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
