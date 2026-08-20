// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class arpgEditorTarget : TargetRules
{
	public arpgEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		ExtraModuleNames.AddRange(new string[] {
			"ARPGCore", "ARPGCombat", "ARPGMagic", "ARPGWorld", "ARPGAI", "arpg",

			// Editor-only, and listed only here: the game target must never
			// build it. It links UnrealEd and GraphEditor.
			"ARPGCombatEditor"
		});
	}
}
