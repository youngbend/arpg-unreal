// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGAIEditorModule.h"

DEFINE_LOG_CATEGORY(LogARPGAIEditor);

/**
 * Empty beyond the log category. The commandlet is discovered by class, so
 * there is nothing for a startup to do -- and an empty StartupModule is better
 * than one that exists to look busy.
 */
IMPLEMENT_MODULE(FARPGAIEditorModule, ARPGAIEditor)
