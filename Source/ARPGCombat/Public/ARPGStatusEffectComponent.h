// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectComponent.h"
#include "GameplayTagContainer.h"
#include "ARPGStatusEffectComponent.generated.h"

/**
 * How a status VFX is placed on whatever is carrying the effect, given bounds
 * measured from that thing's own geometry.
 *
 * This is what keeps ONE authored visual working on a rat, a tree and a barrel
 * alike, instead of needing a hand-fitted variant per silhouette. Authoring
 * cost is per EFFECT, not per effect per shape.
 */
UENUM(BlueprintType)
enum class EARPGStatusVfxFit : uint8
{
	/** At the target's origin, unscaled. */
	None,
	/** Centred on the bounds and scaled to fill them. */
	Bounds,
	/** Sat on the bounds' floor, scaled to their footprint. */
	Base,
	/** Hovering above the bounds -- haloes, marks, auras. */
	Top
};

/**
 * Identity and presentation for a status effect, carried on the GameplayEffect
 * itself.
 *
 * WHY THIS EXISTS. In Godot a status effect was exactly one .tres: combat data,
 * icon, VFX scene, fit mode and priority all in one authored file. The obvious
 * GAS port splits that in two -- a GameplayEffect for duration/stacking and a
 * separate data asset for everything else -- which doubles the authoring burden
 * and lets the two drift apart. Hanging the presentation data off the effect as
 * a component keeps ONE ASSET PER STATUS EFFECT, which is the property worth
 * preserving.
 *
 * The combat behaviour that used to live in GDScript hooks does NOT belong
 * here. Godot's wet.gd granted fire resistance through a hand-rolled timed
 * bonus, with a meta flag to stop repeated applications compounding it; in GAS
 * that is a modifier on the effect and the whole problem evaporates. Reach for
 * a modifier first, and only fall back to code when the behaviour genuinely
 * cannot be expressed as one.
 */
UCLASS(DisplayName = "ARPG Status Effect")
class ARPGCOMBAT_API UARPGStatusEffectComponent : public UGameplayEffectComponent
{
	GENERATED_BODY()

public:
	UARPGStatusEffectComponent();

	/** Identity, e.g. Status.Burning. The effect should also grant this tag. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity",
		meta = (Categories = "Status"))
	FGameplayTag StatusTag;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FText DisplayName;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	TSoftObjectPtr<UTexture2D> Icon;

	/** Drives HUD grouping and any "can this be dispelled" logic. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	bool bIsDebuff = true;

	/**
	 * Damage dealt per period, per stack.
	 *
	 * Declared on the EFFECT rather than taken from whatever applied it, exactly
	 * as tick_damage_type/tick_damage_per_stack were on the .tres. Burning burns
	 * for fire damage whether it was lit by a fireball, a torch, or spreading
	 * grass -- inheriting the applier's damage type would make an identical
	 * burn resisted differently depending on its cause.
	 *
	 * Soft, so declaring one costs no hard content reference from C++.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Tick")
	TSoftObjectPtr<class UARPGDamageTypeAsset> TickDamageType;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Tick",
		meta = (ClampMin = "0.0"))
	float TickDamagePerStack = 0.f;

	/**
	 * Spawned on anything carrying this effect. Null means the effect is
	 * invisible, which is legitimate -- plenty of effects want no visual.
	 *
	 * A soft class rather than a Niagara system so the visual can be an actor
	 * with its own logic, matching Godot's PackedScene contract: the manager
	 * measures the target's bounds and hands them over, so the same asset fits
	 * anything it is attached to.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	TSoftClassPtr<AActor> VfxActorClass;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	EARPGStatusVfxFit VfxFit = EARPGStatusVfxFit::Bounds;

	/** Extra multiplier on the fitted size, for effects that should read larger or smaller. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation",
		meta = (ClampMin = "0.01"))
	float VfxScale = 1.f;

	/**
	 * When more effects want a visual than the budget allows, higher wins. A
	 * character on fire matters more than the same character's faint Weakened
	 * shimmer.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	int32 VfxPriority = 0;

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
};
