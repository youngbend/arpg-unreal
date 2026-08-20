// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGPlaceholderEffect.h"
#include "ARPGSolidField.h"
#include "ARPGSlabEffects.generated.h"

class AARPGSolidBody;
class UDynamicMeshComponent;
class UARPGSolidDefinition;

/**
 * The two shapes a raised slab comes in.
 *
 * Not a radius and a length with one ignored, because which one is meaningless
 * depends on the other -- see how the discharge types differ: a Burst throws up
 * a pillar in front of the caster, an Emanate raises a wall AROUND them.
 */
UENUM(BlueprintType)
enum class EARPGSlabShape : uint8
{
	/** A disc in front of the caster. A pillar, a plug, a block of cover. */
	Pillar,
	/** A bar across the caster's facing. A wall you get behind, not on top of. */
	Wall,
	/** A ring centred on the caster. What an emanation actually emanates. */
	Ring
};

/**
 * A discharge that RAISES A SLAB out of the ground and then gets out of the way.
 *
 * WHY THIS LIVES IN ARPGWorld and not with the other placeholders: a slab is an
 * AARPGSolidBody, which the magic module cannot see. The dependency runs
 * ARPGWorld -> ARPGMagic and this is the class that needs both, so it belongs on
 * the side that can reach across.
 *
 * THE SPELL IS NOT THE SLAB. This actor exists for the moment of casting -- the
 * hitbox that shoves whoever was standing where the ground just erupted, the
 * flash, the sound -- and then dies on its ordinary lifetime. What it raised
 * stays, because it is a separate body that was never owned by the spell. That
 * split is the whole reason a slab did not need a new actor type: an
 * AARPGSolidBody with MeltRate 0 is already a permanent thing standing in the
 * world that reacts, is walked on, and replicates as an outline.
 *
 * NOTHING HERE IS ABOUT EARTH. Point Definition at any solid and this raises it.
 */
UCLASS()
class ARPGWORLD_API AARPGRaiseSlabEffect : public AARPGPlaceholderDischarge
{
	GENERATED_BODY()

public:
	AARPGRaiseSlabEffect();

	virtual void InitializeFromContext(const FARPGDischargeContext& InContext) override;

	/** What the slab IS. Without one, the spell is a shove and nothing rises. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Slab")
	TObjectPtr<UARPGSolidDefinition> Definition;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Slab")
	EARPGSlabShape Shape = EARPGSlabShape::Pillar;

	/**
	 * How far in front of the caster it comes up, in cm.
	 *
	 * Zero puts it underneath them, which for a Ring is right and for a Pillar is
	 * a way to be shoved into the air by your own spell.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Slab",
		meta = (ClampMin = "0.0"))
	float Standoff = 220.f;

	/** Half-width for a Wall, radius for a Pillar, radius for a Ring. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Slab",
		meta = (ClampMin = "10.0"))
	float Extent = 160.f;

	/** How thick a Wall or Ring is front-to-back. Unused by a Pillar. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Slab",
		meta = (ClampMin = "10.0"))
	float Depth = 90.f;

	/**
	 * Scales Extent with the charge, so a held cast raises a bigger one.
	 *
	 * 1 means the charge does nothing. The slab is the payload of this spell and
	 * a charge that only made the flash brighter would be a charge that did
	 * nothing at all.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Slab",
		meta = (ClampMin = "1.0"))
	float ChargeScale = 1.8f;

	/** The slab this spell raised, or null when the ground refused it. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Slab")
	AARPGSolidBody* GetRaised() const { return Raised; }

protected:
	/** Where the ground is under a world position, or false when there is none. */
	bool FindGround(const FVector& From, float& OutHeight) const;

	UPROPERTY(Transient)
	TObjectPtr<AARPGSolidBody> Raised;
};

/**
 * A projectile that throws the slab in front of the caster, if there is one.
 *
 * THE SPELL READS THE WORLD RATHER THAN THE INPUT. Project is one ability shared
 * by every element -- fire, water, lightning -- so "is there a wall in front of
 * me to throw" cannot be a branch there without every other element carrying it.
 * It belongs in the effect, which is per element by construction: the element's
 * DischargeEffects map is what picks this class for Earth and a plain projectile
 * for everything else.
 *
 * IT CONSUMES THE SLAB RATHER THAN FLYING IT. What flies wants a trajectory, a
 * hitbox, a reaction volume and an impact, and a projectile already has all
 * four; a slab is a thing standing in the world. So the wall is destroyed and
 * this projectile grows to account for it -- which is also honest about mass,
 * since the boulder is sized from the volume of rock that actually went.
 */
UCLASS()
class ARPGWORLD_API AARPGLaunchSlabProjectile : public AARPGPlaceholderProjectile
{
	GENERATED_BODY()

public:
	AARPGLaunchSlabProjectile();

	virtual void InitializeFromContext(const FARPGDischargeContext& InContext) override;

	/**
	 * How far ahead to look for something to throw.
	 *
	 * SHORT ON PURPOSE. The point is "cast this at the wall you just raised", not
	 * "the spell hunts for ammunition" -- a caster who wanted a boulder and got
	 * their cover thrown away from across the room has been robbed by their own
	 * spell.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Slab",
		meta = (ClampMin = "0.0"))
	float Reach = 400.f;

	/**
	 * Biggest the boulder may get from eating a slab, as a multiple of its
	 * ordinary radius. A thrown wall is a bigger rock, not a different spell.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Slab",
		meta = (ClampMin = "1.0"))
	float MaxLaunchScale = 2.5f;

	/** True when this went out as a thrown slab rather than a conjured boulder. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Slab")
	bool WasLaunchedFromSlab() const { return bLaunchedFromSlab; }

	// --- Carrying the rock rather than a stand-in --------------------------------
	//
	// A MESH IS RIGHT HERE AND WRONG EVERYWHERE ELSE IN THIS SYSTEM, and the
	// difference is one word: EDITED. Everything against meshes -- unbounded vertex
	// growth, sliver accumulation, a collision cook that gets worse every time, an
	// object you cannot replicate -- is an argument about REPEATED boolean editing,
	// which is the failure the heightfield replaced. A mesh built once, carried,
	// and thrown away has none of it.
	//
	// AND ROTATION IS THE ONE THING A HEIGHTFIELD CANNOT DO. Its columns run along
	// world Z; yaw leaves them vertical (see AARPGSolidBody's frame) but tumbling
	// does not, and a thrown rock tumbles. So the two states want two
	// representations, and the conversions are single events at each end: bake on
	// launch, rebuild on impact.

	/**
	 * The slab this was, kept whole so it can be put back down.
	 *
	 * WHAT LANDS IS WHAT WAS THROWN, melt scars and all. Without the snapshot the
	 * spell would consume a carved pillar and drop a generic one, which is the
	 * sort of detail nobody articulates and everybody notices.
	 */
	UPROPERTY(Transient)
	FARPGSolidField Carried;

	UPROPERTY(Transient)
	TObjectPtr<UARPGSolidDefinition> CarriedDefinition;

	/**
	 * How fast the thrown rock tumbles, in degrees per second on each axis.
	 *
	 * The reason this class exists rather than a projectile with a rock mesh on
	 * it: a slab standing in the world cannot pitch or roll, and one in the air
	 * has to.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Slab")
	FVector TumbleRate = FVector(140.f, 90.f, 60.f);

	/**
	 * Whether the rock is put back down as a slab where it lands.
	 *
	 * On, a thrown pillar becomes cover wherever it comes to rest -- which is a
	 * better spell than a rock that vanishes, and turns Project into a way of
	 * MOVING your cover rather than only spending it. Off, it shatters.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Slab")
	bool bLandsAsSlab = true;

	virtual void Tick(float DeltaTime) override;

	/** Puts the carried slab back into the world. Called when the throw ends. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Slab")
	AARPGSolidBody* PutDown(const FVector& Where);

	/**
	 * What the rock is drawn with in the air. Empty until something is thrown.
	 *
	 * ITS OWN COMPONENT rather than the placeholder's sphere, because the two say
	 * different things: the sphere is honestly a stand-in, and this is the actual
	 * slab that was picked up. Presentation only -- the hitbox is what hurts
	 * things, and a carried mesh that collided would shoulder the caster aside on
	 * the way out.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UDynamicMeshComponent> Carriage;

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

protected:
	UPROPERTY(Transient)
	bool bLaunchedFromSlab = false;

	/** So a throw that is destroyed for any other reason does not drop a wall. */
	UPROPERTY(Transient)
	bool bPutDown = false;
};
