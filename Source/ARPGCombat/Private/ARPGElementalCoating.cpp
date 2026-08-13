// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGElementalCoating.h"
#include "ARPGHitboxComponent.h"

void FARPGElementalCoating::ApplyTo(UARPGHitboxComponent* Coated,
	const UARPGHitboxComponent& SwingHitbox) const
{
	if (!Coated)
	{
		return;
	}

	// The element's half.
	Coated->BaseDamage = BaseDamage;
	Coated->PoiseDamage = PoiseDamage;
	Coated->DamageType = DamageType;
	Coated->MagicElementTag = MagicElementTag;
	Coated->OnHitEffects = OnHitEffects;
	Coated->OnHitEffectDuration = OnHitEffectDuration;

	// Reach, which is the reason this is a hitbox of its own at all.
	Coated->TraceRadius = SwingHitbox.TraceRadius * TraceRadiusScale;

	// The attack's half. Inherited rather than authored on the element: these
	// describe how the SWING behaves, and a coating that filtered factions
	// differently from the blade carrying it would be a bug, not a feature.
	Coated->TraceObjectTypes = SwingHitbox.TraceObjectTypes;
	Coated->bIgnoreFactionFilter = SwingHitbox.bIgnoreFactionFilter;
	Coated->bUnblockable = SwingHitbox.bUnblockable;
	Coated->bOneShot = SwingHitbox.bOneShot;
	Coated->TickInterval = SwingHitbox.TickInterval;
	Coated->DamageEffectClass = SwingHitbox.DamageEffectClass;
	Coated->bDrawDebugTrace = SwingHitbox.bDrawDebugTrace;

	// Penetration is left at the default so the ELEMENT's damage type decides how
	// much resistance it ignores. PenetrationOverride is the weapon's answer for
	// steel and has nothing to say about fire.
	Coated->PenetrationOverride = -1.f;

	// See the comment on ApplyTo: one blow, one shove, one freeze.
	Coated->KnockbackForce = 0.f;
	Coated->HitStopDuration = 0.f;

	// The attack's own bonus crit chance is the weapon's, not the coating's. The
	// wielder's CritChance attribute still applies to both, as it should.
	Coated->CriticalChance = 0.f;
}
