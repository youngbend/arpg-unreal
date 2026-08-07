// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "ARPGGameplayEffectContext.generated.h"

/**
 * The DamageInstance replacement.
 *
 * In Godot, DamageInstance was a RefCounted object created by HitboxComponent
 * and consumed by CombatComponent, carrying everything a hit needed to know
 * about itself. GAS's own FGameplayEffectContext carries instigator, causer,
 * hit result and origin -- but nothing about poise, knockback, hit-stop, or
 * whether the blow can be blocked. Those ride here.
 *
 * REPLICATION. Every field below except Metadata is net-serialised. This is
 * load-bearing under server-authoritative co-op: the server resolves the hit
 * and the client needs the presentation-facing parts (contact point, hit-stop,
 * knockback, crit) to play the right reaction. A field added here without a
 * matching line in NetSerialize arrives silently zeroed on clients, which is a
 * uniquely annoying bug to track down -- add both together.
 */
USTRUCT()
struct ARPGCORE_API FARPGGameplayEffectContext : public FGameplayEffectContext
{
	GENERATED_BODY()

public:
	/**
	 * The hitbox that produced this hit, if any.
	 *
	 * Typed as UActorComponent rather than UARPGHitboxComponent because
	 * ARPGCore sits BELOW ARPGCombat in the module DAG and must not reach up
	 * into it. Phase 1 casts this at the point of use; the layering is worth
	 * more than the static type here.
	 */
	UPROPERTY()
	TWeakObjectPtr<UActorComponent> SourceHitbox;

	/**
	 * World-space point where the strike connected, used to drive the victim's
	 * impact reaction and wound placement.
	 *
	 * bHasContactPoint gates it: not every hit can resolve one, and the world
	 * origin is a legitimate coordinate -- observers must not read an unset
	 * point as (0,0,0). This is the same trap the Godot version documented on
	 * DamageInstance::has_contact_point().
	 */
	UPROPERTY()
	FVector ContactPoint = FVector::ZeroVector;

	UPROPERTY()
	bool bHasContactPoint = false;

	UPROPERTY()
	FVector KnockbackDirection = FVector::ZeroVector;

	UPROPERTY()
	float KnockbackForce = 0.f;

	/** Poise/stance damage this hit contributes to the victim's poise meter. */
	UPROPERTY()
	float PoiseDamage = 0.f;

	/** Seconds of animation freeze applied to BOTH parties when this lands. */
	UPROPERTY()
	float HitStopDuration = 0.f;

	/** Fraction of the target's resistance to ignore. -1 = use the damage type's default. */
	UPROPERTY()
	float Penetration = -1.f;

	/** This attack cannot be blocked or parried -- it must be dodged. */
	UPROPERTY()
	bool bUnblockable = false;

	/**
	 * Rolled on the SERVER ONLY and replicated down. Never roll this on a
	 * client: an independent roll desyncs the damage number from the reaction
	 * that plays alongside it.
	 */
	UPROPERTY()
	bool bIsCritical = false;

	/** Which element delivered this hit, when one did. Replaces metadata["magic_element_id"]. */
	UPROPERTY()
	FGameplayTag MagicElementTag;

	/**
	 * Free-form escape hatch, mirroring DamageInstance::metadata.
	 *
	 * DELIBERATELY NOT REPLICATED. Its one real use in the Godot project was
	 * carrying the magic element id, which is now MagicElementTag above and
	 * properly replicated. What remains is server-side bookkeeping between an
	 * execution and its own observers. Anything a client needs to SEE belongs
	 * in a named, replicated field -- not in here.
	 */
	TMap<FGameplayTag, float> Metadata;

	virtual UScriptStruct* GetScriptStruct() const override
	{
		return FARPGGameplayEffectContext::StaticStruct();
	}

	virtual FARPGGameplayEffectContext* Duplicate() const override
	{
		FARPGGameplayEffectContext* NewContext = new FARPGGameplayEffectContext(*this);
		if (GetHitResult())
		{
			// Deep-copy the hit result, matching the base implementation --
			// the copy constructor above only copies the pointer.
			NewContext->AddHitResult(*GetHitResult(), /*bReset=*/true);
		}
		return NewContext;
	}

	virtual bool NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess) override;

	/** Convenience accessor -- returns null when the context is not one of ours. */
	static const FARPGGameplayEffectContext* ExtractFrom(const FGameplayEffectContextHandle& Handle);
};

template<>
struct TStructOpsTypeTraits<FARPGGameplayEffectContext>
	: public TStructOpsTypeTraitsBase2<FARPGGameplayEffectContext>
{
	enum
	{
		WithNetSerializer = true,
		WithCopy = true
	};
};
