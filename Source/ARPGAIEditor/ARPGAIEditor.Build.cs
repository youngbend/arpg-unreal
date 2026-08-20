// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/// Editor-only tooling for the NPC layer: the StateTree authoring commandlet.
///
/// A SEPARATE MODULE for the same reason ARPGCombatEditor is one -- everything
/// here links StateTreeEditorModule and UnrealEd, which a shipping build does
/// not have. It also exists because StateTree authoring is not reachable from
/// Python on this engine: UStateTreeFactory::StateTreeSchemaClass is protected,
/// UStateTreeEditingSubsystem::CompileStateTree is a plain static rather than a
/// UFUNCTION, and FStateTreePropertyPathBinding is not exposed at all. All three
/// are ordinary C++ from in here.
public class ARPGAIEditor : ModuleRules
{
	public ARPGAIEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"ARPGAI",
			"ARPGCombat",
			"ARPGCore",
			"GameplayStateTreeModule",   // UStateTreeAIComponentSchema
			"StateTreeModule",
			"StateTreeEditorModule",

			// FStateTreeCompilerLog's message type holds an
			// FPropertyBindingBindableStructDescriptor, so its destructor has to
			// link even though nothing here names that type. Without this the
			// module compiles and fails at link with one unresolved external.
			"PropertyBindingUtils",
			"UnrealEd"
		});
	}
}
