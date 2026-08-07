// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/// Magic layer: elements, combination table, magic component, discharge
/// abilities, and ElementalVolume.
///
/// ElementalVolume lives here rather than in ARPGWorld because it genuinely is
/// "a region made of an element". The four SOLVERS that consume it (reaction,
/// conduction, spread, fluid surface) live in ARPGWorld — in the Godot project
/// two of them sat under src/magic/, which would close a dependency cycle here.
public class ARPGMagic : ModuleRules
{
	public ARPGMagic(ReadOnlyTargetRules Target) : base(Target)
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
			"ARPGCombat"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
