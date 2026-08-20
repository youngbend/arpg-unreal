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
			"UMG",
			"Slate",
			"SlateCore",

			// CommonUI. Its console variables were already being set in
			// DefaultGame.ini while the plugin itself was disabled, so three
			// CommonUI.* settings sat there doing nothing -- leftover template
			// config. It earns its place here for a real reason: this game is
			// gamepad-first, and CommonUI exists to own exactly the input-routing
			// and focus problem UARPGHudWidget::NativeOnInitialized documents at
			// length -- Slate consuming face buttons and the D-pad as UI
			// navigation before they ever reach player input.
			"CommonUI",

			// Gameplay Ability System.
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",

			// The NPC behaviour test reads UStateTree::IsReadyToRun to tell an
			// authored tree from a compiled one.
			"StateTreeModule",

			// ARPG runtime modules. The game module sits on top of the whole
			// chain; see Docs/UNREAL_PORT_PLAN.md section 2 for the DAG.
			"ARPGCore",
			"ARPGCombat",
			"ARPGMagic",
			"ARPGWorld",
			"ARPGAI"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		// --- Automation tests ------------------------------------------------
		//
		// The fixtures under Tests/ build on CQTest's FActorTestSpawner, which
		// owns the test world's whole lifecycle -- including the teardown the
		// hand-rolled fixture never did: routing EndPlay to every actor and
		// shutting down the net driver before destroying the world. That
		// omission is what produced the "CleanupWorld called on a world that has
		// begun play, missing call to EndPlay" warning on almost every test.
		//
		// CQTest is a DEVELOPER module, so it is absent from builds that strip
		// developer tools. ARPG_WITH_TESTS ties the tests' own compilation to
		// that same condition: without it the two could disagree in a Test
		// configuration, where WITH_DEV_AUTOMATION_TESTS is still 1 but the
		// module is gone, and the tests would compile against a module that is
		// not there to link.
		bool bWithARPGTests = Target.bBuildDeveloperTools;
		if (bWithARPGTests)
		{
			PrivateDependencyModuleNames.Add("CQTest");
		}
		PublicDefinitions.Add("ARPG_WITH_TESTS=" + (bWithARPGTests ? "1" : "0"));

		PublicIncludePaths.AddRange(new string[] {
			"arpg"
		});
	}
}
