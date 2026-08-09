// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGReactionDefinitions.h"

UAnimMontage* UARPGFlinchDefinition::GetMontageFor(EARPGPoiseResult Result) const
{
	switch (Result)
	{
	case EARPGPoiseResult::LightFlinch: return LightFlinchMontage;
	case EARPGPoiseResult::HeavyFlinch: return HeavyFlinchMontage;
	case EARPGPoiseResult::StanceBreak: return StanceBreakMontage;
	default: return nullptr;
	}
}
