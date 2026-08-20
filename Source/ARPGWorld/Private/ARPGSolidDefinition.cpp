// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGSolidDefinition.h"
#include "ARPGMagicElement.h"

FGameplayTag UARPGSolidDefinition::GetElementTag() const
{
	return Element ? Element->ElementTag : FGameplayTag();
}
