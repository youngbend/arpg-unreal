// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/// NPC layer: perception, StateTree tasks/conditions, NPC definitions.
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
			"AIModule",          // AAIController, UEnvQuery and the navigation queries

			// StateTree replaces behaviour trees; see Docs/UNREAL_PORT_PLAN.md §9.
			// PUBLIC because the task, condition and evaluator structs derive from
			// StateTree base types in their own public headers.
			"StateTreeModule",
			"GameplayStateTreeModule", // UStateTreeAIComponent and its schema

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
