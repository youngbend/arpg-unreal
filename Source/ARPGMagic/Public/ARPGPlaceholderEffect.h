// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGDischargeEffect.h"
#include "ARPGElementTintable.h"
#include "ARPGMagicTypes.h"
#include "GameFramework/Actor.h"
#include "ARPGPlaceholderEffect.generated.h"

class UPointLightComponent;
class UProjectileMovementComponent;

/**
 * Shape and timing for one discharge type's stand-in.
 *
 * Radii are the HITBOX's, in centimetres, and the drawn volume is built from
 * the same numbers -- see AARPGPlaceholderDischarge.
 */
USTRUCT()
struct FARPGPlaceholderShape
{
	GENERATED_BODY()

	/** Thrown forward rather than sitting still, tracing a capsule as it goes. */
	UPROPERTY()
	bool bSwept = false;

	UPROPERTY()
	float MinRadius = 60.f;

	UPROPERTY()
	float MaxRadius = 200.f;

	/** How far a swept shape travels. Ignored otherwise. */
	UPROPERTY()
	float MinLength = 0.f;

	UPROPERTY()
	float MaxLength = 0.f;

	/** Seconds armed at full size, after expanding and before fading. */
	UPROPERTY()
	float Hold = 0.25f;

	/**
	 * Someone else owns how long this lasts.
	 *
	 * True for a cloak, whose duration comes from the element and whose end is
	 * the cloak component's call. Starting a lifetime timer here would make it
	 * vanish mid-cloak, and disarming would stop it damaging what walks into it.
	 */
	UPROPERTY()
	bool bHeld = false;
};

/**
 * The stand-in for a discharge that has no authored effect yet.
 *
 * BUILT LIKE A DEBUG VOLUME, NOT LIKE A VFX, and that is the whole point: the
 * drawn shape and the hitbox are the SAME numbers, so what you see is literally
 * what the attack hits. Particles and bloom would hide the one thing a
 * placeholder is for, which is reading the reach and shape of a spell while
 * tuning it. A placeholder that looked finished would also stop anyone
 * replacing it.
 *
 * It needs no authored content at all -- no mesh, no material, no particle
 * system. That is deliberate: a fallback that itself had to be authored would
 * not be a fallback. The cost is that the volume is drawn with debug lines and
 * is therefore development-only; the tinted light that goes with it is real, so
 * something still shows in a packaged build.
 *
 * **A burst travels rather than being drawn as a static capsule.** The hitbox
 * sweeps a sphere from where it was to where it is, so a burst is honestly a
 * sphere thrown forward -- and moving it makes the drawn volume exact at every
 * instant instead of an approximation of a shape the hitbox cannot express.
 *
 * Colour comes from the element's palette, so a new element is distinguishable
 * at a glance with three colours and no authoring whatsoever.
 */
UCLASS()
class ARPGMAGIC_API AARPGPlaceholderDischarge : public AARPGDischargeEffect, public IARPGElementTintable
{
	GENERATED_BODY()

public:
	AARPGPlaceholderDischarge();

	virtual void InitializeFromContext(const FARPGDischargeContext& InContext) override;
	virtual void Tick(float DeltaTime) override;

	//~ IARPGElementTintable
	virtual void ApplyPalette_Implementation(const UARPGElementPalette* Palette) override;
	//~ End IARPGElementTintable

	/** The shape for a type, or the default for one with nothing authored. */
	static FARPGPlaceholderShape GetShapeFor(EARPGDischargeType Type);

	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	float GetVolumeRadius() const { return Radius; }

	/** How far a swept shape will travel in total. Zero for a static one. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	float GetTravelDistance() const { return TravelDistance; }

	/** Snappy: the point is to see the final size, not to watch it grow. */
	static constexpr float ExpandTime = 0.1f;
	static constexpr float FadeTime = 0.3f;

protected:
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UPointLightComponent> Light;

	FARPGPlaceholderShape Shape;

	float Radius = 60.f;
	float TravelDistance = 0.f;
	float Elapsed = 0.f;

	FVector StartLocation = FVector::ZeroVector;
	FLinearColor VolumeColour = FLinearColor::White;
};

/**
 * The stand-in for a Project discharge: a small orb that travels and dies.
 *
 * SEPARATE from the volume above because a projectile is not a stationary
 * shape. Standing in for one with something that sits at the caster's feet
 * would misrepresent the spell badly enough to be useless -- range, travel time
 * and whether it actually reaches anything are most of what a projectile is,
 * and all three are exactly what needs testing.
 */
UCLASS()
class ARPGMAGIC_API AARPGPlaceholderProjectile : public AARPGDischargeEffect, public IARPGElementTintable
{
	GENERATED_BODY()

public:
	AARPGPlaceholderProjectile();

	virtual void InitializeFromContext(const FARPGDischargeContext& InContext) override;
	virtual void Tick(float DeltaTime) override;

	//~ IARPGElementTintable
	virtual void ApplyPalette_Implementation(const UARPGElementPalette* Palette) override;
	//~ End IARPGElementTintable

	/** Travel speed, before power scaling. Centimetres per second. */
	UPROPERTY(EditDefaultsOnly, Category = "ARPG|Placeholder", meta = (ClampMin = "0.0"))
	float Speed = 2000.f;

	UPROPERTY(EditDefaultsOnly, Category = "ARPG|Placeholder", meta = (ClampMin = "0.0"))
	float MinRadius = 20.f;

	UPROPERTY(EditDefaultsOnly, Category = "ARPG|Placeholder", meta = (ClampMin = "0.0"))
	float MaxRadius = 60.f;

	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	float GetVolumeRadius() const { return Radius; }

protected:
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UProjectileMovementComponent> Movement;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UPointLightComponent> Light;

	float Radius = 20.f;
	FLinearColor VolumeColour = FLinearColor::White;
};

/**
 * The stand-in for a readied element in hand, or an element coating a weapon.
 *
 * Small and unarmed: it damages nothing and is not a discharge at all. What it
 * is for is answering "what have I got readied" at a glance -- which is the
 * single most useful thing to see while testing the modal control scheme, since
 * the whole LT-then-act idea is invisible until something shows the element in
 * hand.
 */
UCLASS()
class ARPGMAGIC_API AARPGPlaceholderAttachedEffect : public AActor, public IARPGElementTintable
{
	GENERATED_BODY()

public:
	AARPGPlaceholderAttachedEffect();

	virtual void Tick(float DeltaTime) override;

	//~ IARPGElementTintable
	virtual void ApplyPalette_Implementation(const UARPGElementPalette* Palette) override;
	//~ End IARPGElementTintable

	UPROPERTY(EditDefaultsOnly, Category = "ARPG|Placeholder", meta = (ClampMin = "0.0"))
	float Radius = 12.f;

protected:
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UPointLightComponent> Light;

	FLinearColor VolumeColour = FLinearColor::White;
};
