// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Templates/SubclassOf.h"
#include "ARPGElementalCoating.generated.h"

class UARPGDamageTypeAsset;
class UARPGHitboxComponent;
class UGameplayEffect;

/**
 * What an element contributes to one swing that it coats: a whole second hit,
 * described as a value.
 *
 * WHY THE COATING IS ITS OWN HIT, AND ITS OWN HITBOX. The weapon deals its
 * physical damage and the coating deals elemental damage on the same swing, as
 * two independent hits. Folding them into one number would force a single damage
 * type through the mitigation pipeline and lose exactly the interaction that
 * makes imbuing worthwhile -- armour resists the steel, resistance resists the
 * fire, and they are not the same number.
 *
 * And they do not have the same REACH. A coating is not painted on the edge; it
 * is a sheath of burning air around the blade, and it is allowed to catch things
 * the steel misses. That is what settles the question of whether this could ride
 * the weapon's own hitbox as a second payload: it could not, because a payload
 * inherits the geometry of whatever carries it, and this needs its own.
 * TraceRadiusScale is the knob, and it is why UARPGHitboxComponent grew swing
 * pairing -- two hitboxes on one swing need to not cancel each other out on the
 * target's i-frames.
 *
 * A VALUE, NOT A COMPONENT. The formula that fills this in is pure and lives on
 * the magic component beside the discharge formulas, so it can be checked
 * without standing up an avatar, an ASC and a granted ability spec.
 */
USTRUCT(BlueprintType)
struct ARPGCOMBAT_API FARPGElementalCoating
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Coating")
	float BaseDamage = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Coating")
	float PoiseDamage = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Coating")
	TObjectPtr<UARPGDamageTypeAsset> DamageType;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Coating",
		meta = (Categories = "Element"))
	FGameplayTag MagicElementTag;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Coating")
	TArray<TSubclassOf<UGameplayEffect>> OnHitEffects;

	/** Overrides each effect's own duration when > 0. As OnHitEffectDuration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Coating",
		meta = (ClampMin = "0.0"))
	float OnHitEffectDuration = 0.f;

	/**
	 * How much wider the coating sweeps than the blade it is on, as a multiple of
	 * the weapon hitbox's own TraceRadius.
	 *
	 * A multiple rather than an absolute size so one authored number reads the
	 * same on a dagger and a greatsword. 1 keeps the coating exactly on the edge;
	 * above that it starts catching things the steel misses, which is the whole
	 * reason this is a separate hitbox.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Coating",
		meta = (ClampMin = "0.01"))
	float TraceRadiusScale = 1.f;

	/** Nothing to deliver -- what an uncoated swing carries. */
	bool IsEmpty() const
	{
		return BaseDamage <= 0.f && PoiseDamage <= 0.f && OnHitEffects.Num() == 0;
	}

	/**
	 * Configures a hitbox to be this coating, mirroring the swing's own hitbox
	 * for everything the coating does not decide for itself.
	 *
	 * Trace object types, faction filtering, one-shot, re-hit interval and
	 * blockability are all INHERITED, because they belong to the attack rather
	 * than the element: a coating on an unblockable swing is unblockable, and a
	 * coating on a drill re-hits on the drill's cadence.
	 *
	 * Knockback and hit-stop are deliberately zeroed instead. One blow is one
	 * shove and one freeze however many payloads it lands, and the physical hit
	 * already owns both.
	 */
	void ApplyTo(UARPGHitboxComponent* Coated, const UARPGHitboxComponent& SwingHitbox) const;
};
