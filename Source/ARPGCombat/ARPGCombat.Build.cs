// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/// Combat layer: ability system component, damage execution, hitbox/hurtbox,
/// poise/parry/block, weapons, armor, combo, progression.
public class ARPGCombat : ModuleRules
{
	public ARPGCombat(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			"ARPGCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
