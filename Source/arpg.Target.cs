// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class arpgTarget : TargetRules
{
	public arpgTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		ExtraModuleNames.AddRange(new string[] {
			"ARPGCore", "ARPGCombat", "ARPGMagic", "ARPGWorld", "ARPGAI", "arpg"
		});
	}
}
