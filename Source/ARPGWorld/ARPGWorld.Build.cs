// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/// World simulation layer: the four elemental solvers, all as UWorldSubsystems
/// and all server-authoritative (see Docs/UNREAL_PORT_PLAN.md §3.6).
///
///   ElementalReactionSystem   two volumes meeting — resolved once
///   ConductionSystem          one charge following a connected medium
///   SpreadSystem              a medium diffusing through the ground
///   FluidSurfaceSystem        an element lying somewhere as a discrete body
///
/// Phase 10 adds GeometryCore/GeometryFramework/DynamicMesh here for the fluid
/// polygon backend. Deliberately not listed yet — unused deps slow builds and
/// the exact 5.8 module names should be confirmed at that point.
public class ARPGWorld : ModuleRules
{
	public ARPGWorld(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			"ARPGCore",
			"ARPGCombat",
			"ARPGMagic"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
