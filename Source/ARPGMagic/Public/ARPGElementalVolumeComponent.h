// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "ARPGElementalVolumeComponent.generated.h"

class UARPGMagicElement;
class UPrimitiveComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FARPGOnVolumeReacted,
	float, Consumed, float, Remaining, UARPGMagicElement*, Product);

DECLARE_MULTICAST_DELEGATE_TwoParams(FARPGOnVolumesMet,
	UARPGElementalVolumeComponent* /*A*/, UARPGElementalVolumeComponent* /*B*/);

/**
 * Marks a region of space as being MADE OF an element, so it can react when it
 * meets another one. Port of Godot's ElementalVolume.
 *
 * TWO CONFIGURATIONS, ONE COMPONENT:
 *
 *   PROJECTILE   finite energy, consumed by reacting. A fireball's volume,
 *                energised from its discharge context.
 *   RESERVOIR    effectively bottomless, and damping: a river, a lava flow.
 *                bReservoir with high Absorption.
 *
 * That single distinction is what makes a fireball hitting another spell behave
 * differently from the same fireball hitting a river, without either case being
 * special-cased anywhere.
 *
 * DETECTION IS PHYSICS OVERLAP, deliberately, unlike the spread system's
 * registry and spatial hash. Projectiles move fast and a reaction needs a
 * frame-accurate contact point, which a 10Hz tick cannot give; and there are
 * only ever a handful of live spells, so the broadphase is the cheap option
 * here rather than the expensive one.
 *
 * THE COLLIDER IS NOT THE WATER. A body of water is authored as a tall box so a
 * spell arriving above it still enters -- the Godot river was 7m deep -- and
 * that box is cut metres into the banks either side. Treating "overlaps the box"
 * as "is in the water" is what shocked someone standing dry on the shore and
 * detonated a fireball flying over the river. So the box stays the BROADPHASE
 * and ContainsPoint is the truth.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGMAGIC_API UARPGElementalVolumeComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UARPGElementalVolumeComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	// --- What it is -----------------------------------------------------------

	/** Reactions need an element on both sides; a volume without one is inert. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Volume")
	TObjectPtr<UARPGMagicElement> Element;

	/**
	 * How much there is to react with, in the same units as a discharge's
	 * ComputedDamage -- which is what a projectile should set this from, so the
	 * reaction already accounts for charge, mastery and buffs without
	 * re-deriving any of them.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Volume")
	void SetEnergy(float NewEnergy);

	UFUNCTION(BlueprintPure, Category = "ARPG|Volume")
	float GetEnergy() const { return Energy; }

	/** Retained so Remaining can be a fraction after repeated partial reactions. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Volume")
	float GetInitialEnergy() const { return InitialEnergy; }

	/**
	 * A reservoir is not depleted by reacting: a river does not run out. Its
	 * energy is still read, and should be large, since it decides which side of
	 * a collision survives.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Volume")
	bool bReservoir = false;

	/**
	 * 0-1. How much reacted energy this soaks up as heat instead of releasing
	 * visibly.
	 *
	 * The knob that makes a fireball hitting a river a hiss of steam rather than
	 * an explosion: the water is there in such quantity that it carries the
	 * energy away. Projectiles leave it at 0.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Volume",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Absorption = 0.f;

	/**
	 * 0-1. How well this carries a charge that conducts THROUGH it. 0 -- the
	 * default, and every projectile -- means it carries nothing and drops out of
	 * a flood entirely; a river sits near 0.85.
	 *
	 * WHICH charges it carries is not a property of the volume: that is a
	 * Conduct row in the shared combination table. This is only how well this
	 * particular piece of world does it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Volume",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Conductivity = 0.f;

	/**
	 * Whether this is something you can stand IN rather than something thrown AT
	 * you, and so applies its element's ambient damage and status to whoever is
	 * inside.
	 *
	 * Separate from bReservoir, which answers a genuinely different question --
	 * "can this be depleted". Those coincided while a river was the only
	 * standing body in the game; a puddle is a standing body a fireball can
	 * absolutely boil away, and gating on bReservoir made small pools intangible
	 * to stand in.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Volume")
	bool bAmbientSource = false;

	/** Seconds between ambient applications. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Volume",
		meta = (ClampMin = "0.05"))
	float AmbientInterval = 1.f;

	/**
	 * Surface height relative to the component, for a reservoir. The waterline
	 * inside the much taller broadphase box -- see the class comment.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Volume",
		meta = (EditCondition = "bReservoir"))
	float SurfaceHeightOffset = 0.f;

	/**
	 * The collider whose overlaps define this volume. Defaults to the owner's
	 * root primitive.
	 *
	 * Explicit rather than derived-from-self because the shape differs by case:
	 * a spell is a sphere, a river is a long box. One component serving both
	 * beats a sphere-shaped volume class and a box-shaped one that share all
	 * their behaviour.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Volume")
	TObjectPtr<UPrimitiveComponent> OverlapSource;

	/** Damage from a reaction still attributes to whoever cast the spell. */
	UPROPERTY(BlueprintReadWrite, Category = "ARPG|Volume")
	TObjectPtr<AActor> SourceActor;

	// --- Reacting -------------------------------------------------------------

	/**
	 * Deducts energy (no-op on a reservoir) and notifies the owner. Stops
	 * monitoring once fully spent, so a consumed projectile cannot react again
	 * while its effect plays out.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Volume")
	void Consume(float Amount, UARPGMagicElement* Product);

	/** The other half of Consume: this volume ATE what it met and grew. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Volume")
	void Amplify(float Amount, UARPGMagicElement* Product);

	/**
	 * Is this point genuinely INSIDE, rather than merely within the broadphase?
	 *
	 * @param BelowReach extends the test downward by the querying thing's own
	 *        reach, so a character is in the water when their FEET are, not
	 *        their centre, and a projectile when its underside has touched.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Volume")
	bool ContainsPoint(FVector WorldPoint, float BelowReach = 0.f) const;

	/** Waterline for a reservoir, the top of the bounds otherwise. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Volume")
	float GetSurfaceHeightAt(FVector WorldPoint) const;

	/**
	 * Where this volume actually IS: the centre of its collider, not of the
	 * component.
	 *
	 * They are the same thing only when the component happens to sit at the
	 * shape's origin, and nothing enforces that -- a volume added to an actor
	 * without being attached sits at the world origin while its collider is
	 * metres away. Every geometric answer (contact points, conduction hop
	 * distances, immersion) has to come from the SHAPE, or a reaction resolves
	 * against a position nothing is at.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Volume")
	FVector GetVolumeLocation() const;

	/** Every other volume currently overlapping this one. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Volume")
	TArray<UARPGElementalVolumeComponent*> GetOverlappingVolumes() const;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Volume")
	FARPGOnVolumeReacted OnReacted;

	/**
	 * Raised whenever two volumes meet. The reaction subsystem subscribes; this
	 * component never decides what two elements do to each other.
	 *
	 * WHY A STATIC DELEGATE RATHER THAN A DIRECT CALL. The solvers live in
	 * ARPGWorld because phase 10 needs them to reach the fluid system, and
	 * ARPGWorld already depends on ARPGMagic -- so a volume calling the reaction
	 * subsystem directly would close the cycle. Inverting it here means the
	 * dependency still runs one way and a volume stays a description of what a
	 * region is made of, with no knowledge of who resolves it.
	 *
	 * Subscribers MUST filter by world: this is process-wide, so a PIE session
	 * with a server and a client world would otherwise cross-talk.
	 */
	static FARPGOnVolumesMet OnVolumesMet;

protected:
	UFUNCTION()
	void HandleOverlapBegin(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep,
		const FHitResult& SweepResult);

private:
	/** Reports the reaction to the owner, or applies the default if it has no hook. */
	void NotifyOwner(float Consumed, float Remaining, UARPGMagicElement* Product);

	/**
	 * What an effect gets for free when it implements no hook: hitboxes scaled by
	 * the remaining fraction, and the actor scaled by its CUBE ROOT -- so the
	 * thing loses that fraction of its VOLUME rather than its width, and the
	 * visible change matches the power change instead of overstating it.
	 */
	void ApplyDefaultReaction(float Remaining);

	/**
	 * A reservoir simply BEING what it is made of, applied to whatever stands in
	 * it -- a river wets you, a lava flow burns you -- independent of any
	 * reaction.
	 */
	void ApplyAmbient();

	/**
	 * A reservoir polling what is inside it descend.
	 *
	 * Overlap fires ONCE, on entry -- and a body of fluid is entered metres
	 * above its own surface, because it is a tall box with headroom. So entry is
	 * the wrong moment to react, and nothing fires when the thing finally
	 * reaches the water. This resolves each overlap the moment it actually
	 * arrives.
	 */
	void PollImmersion();

	float Energy = 0.f;
	float InitialEnergy = 0.f;
	bool bSpentAny = false;
	bool bMonitoring = true;

	float AmbientAccumulator = 0.f;

	/**
	 * Captured before the first reaction, so scaling is absolute rather than
	 * compounding. Multiplying the CURRENT scale by the remaining fraction each
	 * time -- which the Godot version did -- shrinks a twice-reacted volume
	 * faster than its energy actually fell.
	 */
	FVector BaseScale = FVector::OneVector;
	bool bCapturedBaseScale = false;
};
