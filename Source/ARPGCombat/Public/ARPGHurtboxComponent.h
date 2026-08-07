// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayEffectTypes.h"
#include "ARPGHurtboxComponent.generated.h"

class UAbilitySystemComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FARPGOnHitReceived,
	const FGameplayEffectContextHandle&, Context, float, Magnitude);

/**
 * Marks an actor as damageable and owns its invincibility window.
 *
 * A UActorComponent, NOT a collision volume -- a deliberate change from Godot's
 * Area3D hurtbox.
 *
 * In Godot the hurtbox had to BE the collision shape, because detection was
 * area-overlap and something had to overlap. Here detection is swept traces
 * (see UARPGHitboxComponent), and traces already hit whatever collision the
 * actor has -- in practice a skeletal mesh's physics asset. Making the hurtbox
 * a second, coarser volume layered over that would throw away the per-bone
 * information the physics asset already gives us, and force every character to
 * carry a hand-placed capsule that has to be kept in sync with its mesh.
 *
 * So the trace hits real geometry, and this component answers the questions
 * geometry cannot: is this thing damageable, is it currently invincible, and
 * which ability system component should the hit be routed to.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCOMBAT_API UARPGHurtboxComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGHurtboxComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/** Seconds of invincibility granted after each hit lands. 0 = none. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hurtbox",
		meta = (ClampMin = "0.0"))
	float InvincibilityDuration = 0.f;

	/**
	 * Forced invincibility, independent of the timed window -- what a dodge
	 * roll's i-frames set. Phase 3 drives this from the State.Invulnerable tag.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Hurtbox")
	void SetForceInvincible(bool bNewInvincible) { bForceInvincible = bNewInvincible; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Hurtbox")
	bool IsInvincible() const { return bForceInvincible || InvincibilityTimer > 0.f; }

	/** The ASC hits are routed to. Cached from the owner via IAbilitySystemInterface. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Hurtbox")
	UAbilitySystemComponent* GetAbilitySystemComponent() const;

	/**
	 * Consumes the invincibility check and starts a fresh window.
	 * Returns false when the hit should be ignored.
	 *
	 * Separate from actually applying damage so the hitbox decides what to apply
	 * while the hurtbox decides whether anything may land at all -- the same
	 * split that let Godot's SpreadSystem deal environmental damage through
	 * receive_hit() and still respect dodge i-frames.
	 */
	bool TryConsumeHit();

	/** Fired on the server after a hit is applied. For VFX/sound/UI. */
	UPROPERTY(BlueprintAssignable, Category = "ARPG|Hurtbox")
	FARPGOnHitReceived OnHitReceived;

private:
	UPROPERTY(Transient)
	mutable TObjectPtr<UAbilitySystemComponent> CachedASC;

	float InvincibilityTimer = 0.f;
	bool bForceInvincible = false;
};
