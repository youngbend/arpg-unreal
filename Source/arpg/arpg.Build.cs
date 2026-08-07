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
			"Slate"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		PublicIncludePaths.AddRange(new string[] {
			"arpg",
			"arpg/Variant_Platforming",
			"arpg/Variant_Platforming/Animation",
			"arpg/Variant_Combat",
			"arpg/Variant_Combat/AI",
			"arpg/Variant_Combat/Animation",
			"arpg/Variant_Combat/Gameplay",
			"arpg/Variant_Combat/Interfaces",
			"arpg/Variant_Combat/UI",
			"arpg/Variant_SideScrolling",
			"arpg/Variant_SideScrolling/AI",
			"arpg/Variant_SideScrolling/Gameplay",
			"arpg/Variant_SideScrolling/Interfaces",
			"arpg/Variant_SideScrolling/UI"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
