// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ARPGDischargeContext.h"
#include "ARPGDischargeEffect.generated.h"

class UARPGHitboxComponent;

/**
 * What a cast spell actually is in the world: the fireball, the emanation, the
 * cloak shell.
 *
 * WHY THE HITBOX IS STAMPED HERE RATHER THAN AUTHORED PER EFFECT. In Godot every
 * discharge scene carried its own hitbox and its own copy of "read the damage
 * off the context and set it" -- twenty-odd scenes each reimplementing the same
 * three lines, which is where a spell quietly dealing base damage instead of
 * charged damage came from. Provisioning happens once, at the spawn point, so an
 * effect asset is only ever art plus its own movement.
 *
 * Blueprint subclasses override OnDischargeInitialized for the art. They do not
 * need to touch damage at all.
 */
UCLASS(Blueprintable)
class ARPGMAGIC_API AARPGDischargeEffect : public AActor
{
	GENERATED_BODY()

public:
	AARPGDischargeEffect();

	/**
	 * Called by the discharge ability immediately after spawning, before
	 * BeginPlay finishes deferred spawning. Stamps the hitbox and then hands off
	 * to Blueprint.
	 */
	void InitializeFromContext(const FARPGDischargeContext& InContext);

	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	const FARPGDischargeContext& GetDischargeContext() const { return Context; }

	/** The art hook. Damage is already configured by the time this runs. */
	UFUNCTION(BlueprintImplementableEvent, Category = "ARPG|Magic",
		meta = (DisplayName = "On Discharge Initialized"))
	void OnDischargeInitialized(const FARPGDischargeContext& InContext);

	/**
	 * Arms the hitbox for this many seconds, then disarms. 0 leaves it armed for
	 * the actor's whole life, which is what a lingering emanation wants.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Magic",
		meta = (ClampMin = "0.0"))
	float HitboxWindow = 0.f;

	/** Destroyed after this long. 0 means the effect manages its own lifetime. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Magic",
		meta = (ClampMin = "0.0"))
	float Lifetime = 5.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGHitboxComponent> Hitbox;

protected:
	virtual void BeginPlay() override;

	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Magic")
	FARPGDischargeContext Context;
};
