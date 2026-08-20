// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/// Combat layer: ability system component, damage execution, hitbox/hurtbox,
/// poise/parry/block, weapons, armor, combo, progression.
public class ARPGCombat : ModuleRules
{
	public ARPGCombat(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// GameplayAbilities and GameplayTags stay PUBLIC: this module's headers
		// name FActiveGameplayEffectHandle, UGameplayEffect and FGameplayTag in
		// their own signatures, so anything including them needs both.
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"GameplayAbilities",
			"GameplayTags",
			"ARPGCore"
		});

		// PRIVATE: ability tasks are used inside the melee ability's .cpp and
		// appear in no header here, so downstream modules do not inherit it.
		PrivateDependencyModuleNames.AddRange(new string[] {
			"GameplayTasks",
			"Niagara"
		});
	}
}
