// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "GameplayTagContainer.h"
#include "Engine/EngineTypes.h"
#include "ARPGCombatTypes.h"
#include "ARPGHitboxComponent.generated.h"

class UARPGDamageTypeAsset;
class UARPGHurtboxComponent;
class UGameplayEffect;
class UAbilitySystemComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FARPGOnHitLanded,
	AActor*, HitActor, const FHitResult&, Hit);

/**
 * Delivers damage along a swing. Port of Godot's HitboxComponent.
 *
 * DETECTION CHANGED, THE CONTRACT DID NOT. Godot armed an Area3D and reacted to
 * overlap events. This sweeps a sphere between the component's previous and
 * current world position each tick instead. That is not a stylistic preference:
 * an overlap volume moving fast enough passes through a target between two
 * frames without ever generating an event, and the Godot version already had to
 * publish hitbox_world_position every frame to work around it. A sweep cannot
 * miss the gap because the gap is what it traces.
 *
 * Everything else is preserved: one hit per target per activation, optional
 * re-hit ticking for channelled attacks, one-shot, faction filtering, and the
 * deferred self-overlap rule.
 *
 * SERVER-AUTHORITATIVE. Traces run and damage applies only with authority. The
 * component still ticks on clients so cosmetic hooks can fire, but nothing it
 * does there is allowed to change gameplay state.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCOMBAT_API UARPGHitboxComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UARPGHitboxComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	// --- Damage payload -------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox|Damage")
	float BaseDamage = 10.f;

	/**
	 * Which hitbox this is, so an attack can arm the blade or the body without
	 * the ability having to guess between several on the same character.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox")
	EARPGHitboxSource HitboxSource = EARPGHitboxSource::Weapon;

	/**
	 * The weapon's own damage, before the swing's motion value scales it.
	 *
	 * Separate from BaseDamage because BaseDamage is REWRITTEN every time an
	 * attack arms this hitbox (weapon damage x motion value). Multiplying
	 * BaseDamage in place would compound it with every swing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox|Damage",
		meta = (ClampMin = "0.0"))
	float WeaponBaseDamage = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox|Damage")
	TObjectPtr<UARPGDamageTypeAsset> DamageType;

	/** Overrides the damage type's DefaultPenetration when >= 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox|Damage",
		meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float PenetrationOverride = -1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox|Damage")
	float PoiseDamage = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox|Damage")
	float KnockbackForce = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox|Damage")
	float HitStopDuration = 0.f;

	/**
	 * Per-attack crit chance, ADDED to the attacker's CritChance attribute rather
	 * than replacing it -- a heavy finisher is authored as "+15% on top of whatever
	 * this character rolls with", so it stays meaningful across every weapon.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox|Damage",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CriticalChance = 0.f;

	/** Cannot be blocked or parried; must be dodged. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox|Damage")
	bool bUnblockable = false;

	/** Element that delivered the hit, when one did. Stamped onto the context. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox|Damage",
		meta = (Categories = "Element"))
	FGameplayTag MagicElementTag;

	/** The effect carrying UARPGDamageExecution. Defaults to UARPGDamageGameplayEffect. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox|Damage")
	TSubclassOf<UGameplayEffect> DamageEffectClass;

	// --- Activation behaviour -------------------------------------------------

	/** Stop monitoring after the first successful hit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox")
	bool bOneShot = false;

	/**
	 * Seconds between re-hits of a still-overlapping target. 0 = a target is only
	 * ever hit once per activation. Set for channelled/drill attacks that hold a
	 * single contact for the whole swing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox",
		meta = (ClampMin = "0.0"))
	float TickInterval = 0.f;

	/** Skip faction filtering -- a hazard that should also hurt its summoner. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox")
	bool bIgnoreFactionFilter = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox",
		meta = (ClampMin = "0.0"))
	float TraceRadius = 20.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox")
	TArray<TEnumAsByte<EObjectTypeQuery>> TraceObjectTypes;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hitbox")
	bool bDrawDebugTrace = false;

	// --- Control --------------------------------------------------------------

	/**
	 * Arms the hitbox and starts a fresh activation: the per-activation hit set
	 * is cleared, and anything belonging to the source that is ALREADY
	 * overlapping is deferred (see DeferredSelfTargets).
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Hitbox")
	void ActivateHitbox();

	UFUNCTION(BlueprintCallable, Category = "ARPG|Hitbox")
	void DeactivateHitbox();

	UFUNCTION(BlueprintPure, Category = "ARPG|Hitbox")
	bool IsHitboxActive() const { return bArmed; }

	/**
	 * Who the damage is attributed to. Defaults to the owning actor, which is
	 * wrong for a weapon actor -- set this to the wielder so a sword does not
	 * self-attribute its own swings.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Hitbox")
	void SetSourceActor(AActor* NewSource) { SourceActorOverride = NewSource; }

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Hitbox")
	FARPGOnHitLanded OnHitLanded;

private:
	bool bArmed = false;
	FVector PreviousLocation = FVector::ZeroVector;
	float TickAccumulator = 0.f;

	UPROPERTY(Transient)
	TWeakObjectPtr<AActor> SourceActorOverride;

	/** Damaged during this activation. Cleared on arm, and every TickInterval. */
	TSet<TWeakObjectPtr<AActor>> HitTargets;

	/**
	 * Targets belonging to the source that were already overlapping when this
	 * activation began -- a magma patch cast underfoot, a nova centred on the
	 * caster. Suppressed so the source is not hit by its own attack on spawn, and
	 * removed once they stop overlapping, so a caster who wanders back into their
	 * own lingering hazard is fair game again.
	 *
	 * Only affects the SOURCE's own actors. Enemies already standing inside a
	 * close-range burst at spawn are hit immediately, as normal.
	 */
	TSet<TWeakObjectPtr<AActor>> DeferredSelfTargets;

	AActor* ResolveSourceActor() const;
	UAbilitySystemComponent* ResolveSourceASC() const;
	void PerformSweep();
	void DeliverHit(UARPGHurtboxComponent* Hurtbox, const FHitResult& Hit);
};
