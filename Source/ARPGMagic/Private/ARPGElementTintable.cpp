// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGElementTintable.h"
#include "ARPGMagicElement.h"
#include "GameFramework/Actor.h"

void ARPGElementTint::Apply(AActor* Instance, const UARPGMagicElement* Element)
{
	if (!Instance || !Element || !Instance->Implements<UARPGElementTintable>())
	{
		return;
	}

	// The palette itself may be null, and that is passed through rather than
	// filtered out here: "this element has no palette" is a thing an effect may
	// legitimately want to know, and the alternative is a silent no-op that
	// leaves the effect wondering whether it was ever asked.
	IARPGElementTintable::Execute_ApplyPalette(Instance, Element->Palette);
}
