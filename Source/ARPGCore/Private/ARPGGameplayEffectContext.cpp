// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGGameplayEffectContext.h"

const FARPGGameplayEffectContext* FARPGGameplayEffectContext::ExtractFrom(
	const FGameplayEffectContextHandle& Handle)
{
	return static_cast<const FARPGGameplayEffectContext*>(Handle.Get());
}

bool FARPGGameplayEffectContext::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	// NOTE (Iris): the engine's own FGameplayEffectContext::NetSerialize carries a
	// warning that changes must be mirrored into FGameplayEffectContextNetSerializer
	// for Iris replication. If this project ever switches to Iris, this subclass
	// needs a matching net serializer registered or these fields silently stop
	// replicating. Generic (non-Iris) replication is what we rely on today.
	FGameplayEffectContext::NetSerialize(Ar, Map, bOutSuccess);

	// Presence mask, so a hit that carries none of the optional payload costs
	// one byte rather than forty. Ordering here must stay in lockstep with the
	// reads below.
	uint8 RepBits = 0;
	if (Ar.IsSaving())
	{
		if (SourceHitbox.IsValid())            { RepBits |= 1 << 0; }
		if (bHasContactPoint)                  { RepBits |= 1 << 1; }
		if (!KnockbackDirection.IsNearlyZero()
			|| KnockbackForce != 0.f)          { RepBits |= 1 << 2; }
		if (PoiseDamage != 0.f)                { RepBits |= 1 << 3; }
		if (HitStopDuration != 0.f)            { RepBits |= 1 << 4; }
		if (Penetration >= 0.f)                { RepBits |= 1 << 5; }
		if (MagicElementTag.IsValid())         { RepBits |= 1 << 6; }
	}

	Ar.SerializeBits(&RepBits, 7);

	// The two standalone flags are cheap enough to always send.
	uint8 Flags = 0;
	if (Ar.IsSaving())
	{
		Flags = (bUnblockable ? 1 : 0) | (bIsCritical ? 2 : 0);
	}
	Ar.SerializeBits(&Flags, 2);
	if (Ar.IsLoading())
	{
		bUnblockable = (Flags & 1) != 0;
		bIsCritical  = (Flags & 2) != 0;
	}

	if (RepBits & (1 << 0))
	{
		Ar << SourceHitbox;
	}
	if (RepBits & (1 << 1))
	{
		ContactPoint.NetSerialize(Ar, Map, bOutSuccess);
		if (Ar.IsLoading())
		{
			bHasContactPoint = true;
		}
	}
	else if (Ar.IsLoading())
	{
		bHasContactPoint = false;
	}
	if (RepBits & (1 << 2))
	{
		KnockbackDirection.NetSerialize(Ar, Map, bOutSuccess);
		Ar << KnockbackForce;
	}
	if (RepBits & (1 << 3))
	{
		Ar << PoiseDamage;
	}
	if (RepBits & (1 << 4))
	{
		Ar << HitStopDuration;
	}
	if (RepBits & (1 << 5))
	{
		Ar << Penetration;
	}
	else if (Ar.IsLoading())
	{
		// -1 is "use the damage type's default", which is not the same as 0
		// ("ignore no resistance") -- restore the sentinel, don't leave a zero.
		Penetration = -1.f;
	}
	if (RepBits & (1 << 6))
	{
		MagicElementTag.NetSerialize(Ar, Map, bOutSuccess);
	}

	// Metadata is deliberately absent -- see the header.

	bOutSuccess = true;
	return true;
}
