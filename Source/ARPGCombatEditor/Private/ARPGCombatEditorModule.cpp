// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGCombatEditorModule.h"

DEFINE_LOG_CATEGORY(LogARPGCombatEditor);

/**
 * Deliberately empty beyond the log category.
 *
 * The asset type registers itself through UAssetDefinition_ARPGWeaponAttackTree,
 * which the engine discovers by class rather than by a startup call, and the
 * graph nodes render through the default SGraphNode factory. There is nothing
 * left for a module startup to do -- and an empty StartupModule is better than
 * one that exists to look busy.
 */
IMPLEMENT_MODULE(FARPGCombatEditorModule, ARPGCombatEditor)
