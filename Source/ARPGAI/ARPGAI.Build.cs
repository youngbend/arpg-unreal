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
			"AIModule",          // BT task/decorator base classes are in public headers
			"GameplayAbilities", // UARPGNPCComponent's header names FGameplayEffectContextHandle
			"GameplayTags",
			"ARPGCore",
			"ARPGCombat"
		});

		// PRIVATE: pathing and gameplay tasks are only reached from .cpp here.
		PrivateDependencyModuleNames.AddRange(new string[] {
			"NavigationSystem",
			"GameplayTasks"
		});
	}
}
