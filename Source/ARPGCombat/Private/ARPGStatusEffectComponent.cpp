// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGStatusEffectComponent.h"
#include "GameplayEffect.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#define LOCTEXT_NAMESPACE "ARPGStatusEffect"

UARPGStatusEffectComponent::UARPGStatusEffectComponent()
{
#if WITH_EDITORONLY_DATA
	EditorFriendlyName = TEXT("ARPG Status Effect");
#endif
}

#if WITH_EDITOR
EDataValidationResult UARPGStatusEffectComponent::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	if (!StatusTag.IsValid())
	{
		Context.AddError(LOCTEXT("NoStatusTag",
			"Status effect has no StatusTag. Nothing can query for this effect without one."));
		Result = EDataValidationResult::Invalid;
	}

	// The tag is how everything else finds this effect -- SpreadSystem keys
	// entirely on "does the target own Status.Burning", never on effect class --
	// so an effect that identifies as a status but does not grant its own tag is
	// invisible to every consumer. Caught here rather than at runtime, where it
	// presents as fire mysteriously refusing to spread.
	if (const UGameplayEffect* Effect = GetOwner())
	{
		if (StatusTag.IsValid() && !Effect->GetGrantedTags().HasTagExact(StatusTag))
		{
			Context.AddError(FText::Format(
				LOCTEXT("TagNotGranted",
					"Status effect declares StatusTag '{0}' but does not grant it. "
					"Add it to the effect's granted tags, or nothing will be able to "
					"detect this status."),
				FText::FromString(StatusTag.ToString())));
			Result = EDataValidationResult::Invalid;
		}
	}

	return Result;
}
#endif

#undef LOCTEXT_NAMESPACE
