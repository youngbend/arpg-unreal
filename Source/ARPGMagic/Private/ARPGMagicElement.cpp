// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGMagicElement.h"
#include "ARPGElementPalette.h"
#include "ARPGMagic.h"
#include "ARPGMagicSettings.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

namespace
{
	/**
	 * Loads a soft class, treating "authored but broken" the same as "not
	 * authored yet".
	 *
	 * That equivalence is the whole point. A reference to a deleted asset still
	 * looks like a filled slot, so the placeholder fallback never triggers and
	 * the spell is cast with nothing in the world -- no projectile, no hitbox,
	 * no reaction, and no error anyone connects back to the spell. That failure
	 * is invisible in a way a grey placeholder is not.
	 */
	TSubclassOf<AActor> LoadOrNull(const TSoftClassPtr<AActor>& Soft)
	{
		if (Soft.IsNull())
		{
			return nullptr;
		}

		UClass* Loaded = Soft.LoadSynchronous();
		if (!Loaded)
		{
			UE_LOG(LogARPGMagic, Warning,
				TEXT("Effect class '%s' is referenced but would not load; falling back to the placeholder."),
				*Soft.ToString());
		}
		return Loaded;
	}
}

TSubclassOf<AActor> UARPGMagicElement::ResolveDischargeEffect(EARPGDischargeType Type) const
{
	if (const TSoftClassPtr<AActor>* Authored = DischargeEffects.Find(Type))
	{
		if (TSubclassOf<AActor> Loaded = LoadOrNull(*Authored))
		{
			return Loaded;
		}
	}

	// No palette means the author has opted out of placeholders entirely, so
	// stay invisible and say so rather than substituting a colourless stand-in
	// that reads as a finished effect.
	if (!Palette)
	{
		UE_LOG(LogARPGMagic, Warning,
			TEXT("Element '%s' has no effect for discharge type %d and no palette to build a placeholder from; "
			     "this discharge will be invisible."),
			*ElementTag.ToString(), static_cast<int32>(Type));
		return nullptr;
	}

	const UARPGMagicSettings* Settings = GetDefault<UARPGMagicSettings>();

	// A projectile has to travel and die on impact; a stationary puff at the
	// caster's feet would misrepresent it badly enough to be useless for testing.
	const TSoftClassPtr<AActor>& Placeholder = (Type == EARPGDischargeType::Project)
		? Settings->PlaceholderProjectile
		: Settings->PlaceholderDischarge;

	return LoadOrNull(Placeholder);
}

TSubclassOf<AActor> UARPGMagicElement::ResolveHandEffect() const
{
	if (TSubclassOf<AActor> Loaded = LoadOrNull(HandEffect))
	{
		return Loaded;
	}
	return Palette ? LoadOrNull(GetDefault<UARPGMagicSettings>()->PlaceholderHand) : nullptr;
}

UARPGAttackDefinition* UARPGMagicElement::GetImbueAttack(EARPGAttackInput Input) const
{
	if (ImbueType != EARPGImbueType::SpecificAttack)
	{
		return nullptr;
	}

	switch (Input)
	{
	case EARPGAttackInput::Light:   return ImbueAttackLight;
	case EARPGAttackInput::Heavy:   return ImbueAttackHeavy;
	case EARPGAttackInput::Special: return ImbueAttackSpecial;
	default:                        return nullptr;
	}
}

TSubclassOf<AActor> UARPGMagicElement::ResolveImbueEffect() const
{
	if (TSubclassOf<AActor> Loaded = LoadOrNull(ImbueEffect))
	{
		return Loaded;
	}
	return Palette ? LoadOrNull(GetDefault<UARPGMagicSettings>()->PlaceholderImbue) : nullptr;
}

#if WITH_EDITOR
EDataValidationResult UARPGMagicElement::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	// Everything keys off the tag -- combination rows, progression, conduction --
	// so an untagged element silently matches nothing rather than failing loudly.
	if (!ElementTag.IsValid())
	{
		Context.AddError(FText::FromString(
			TEXT("ElementTag is unset. Combination rows and progression both key off it, "
			     "so this element can never combine or be tracked.")));
		Result = EDataValidationResult::Invalid;
	}

	if (!DamageType)
	{
		Context.AddWarning(FText::FromString(
			TEXT("No DamageType set. Every discharge, imbue and dodge from this element will "
			     "resolve with no resistance or category, which is almost never intended.")));
	}

	// Partial authoring is legal and useful -- one move now, more later -- but
	// NONE of them means the element is flagged SpecificAttack and behaves in
	// every way like a Continuation one, which is the sort of thing that gets
	// discovered from a player report rather than from the asset.
	if (ImbueType == EARPGImbueType::SpecificAttack
		&& !ImbueAttackLight && !ImbueAttackHeavy && !ImbueAttackSpecial)
	{
		Context.AddWarning(FText::FromString(
			TEXT("ImbueType is SpecificAttack but no imbue attack is set for any button, so "
			     "every swing will fall back to coating the weapon's own attack. Set at least "
			     "one of ImbueAttackLight/Heavy/Special, or use Continuation.")));
	}

	return Result;
}
#endif
