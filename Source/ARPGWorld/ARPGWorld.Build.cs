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
/// The fluid polygon backend uses GeometryCore + GeometryAlgorithms. The plan
/// expected raw Clipper2, which IS vendored under GeometryProcessing but sits in
/// GeometryAlgorithms/Private and cannot be included from here. Its public
/// wrapper is a better fit anyway: PolygonsUnion / PolygonsOffset /
/// PolygonsIntersection / PolygonsDifference already speak in FGeneralPolygon2d,
/// which is exactly the polygon-with-holes type the fluid design wants.
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
			"ARPGMagic",
			"GeometryCore",
			"GeometryAlgorithms"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
