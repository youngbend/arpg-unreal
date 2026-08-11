// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ARPGDischargeContext.h"
#include "ARPGDischargeEffect.generated.h"

class AARPGDischargeEffect;
class UARPGHitboxComponent;

DECLARE_MULTICAST_DELEGATE_OneParam(FARPGOnDischargeLanded,
	AARPGDischargeEffect* /*Effect*/);

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
	virtual void InitializeFromContext(const FARPGDischargeContext& InContext);

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

	// --- What it leaves behind --------------------------------------------------

	/**
	 * Radius of the body of its own element this leaves lying on the ground when
	 * it finishes. 0 -- the default -- leaves nothing.
	 *
	 * NOT "does water make puddles". Whether an element pools at all is the fluid
	 * system's question, answered by whether a fluid definition describes it: set
	 * this on a fire orb and it deposits nothing, with no branch here and no list
	 * of wet elements to keep in sync. What this number says is only how much
	 * GROUND this particular spell covers.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Magic|Deposit",
		meta = (ClampMin = "0.0"))
	float DepositRadius = 0.f;

	/**
	 * Whether the footprint sweeps from where the spell was cast to where it
	 * finished, rather than being a disc at the finish.
	 *
	 * For a JET or a beam, whose body is elongated at any one instant, the stadium
	 * is the honest footprint. For a projectile it is not: a bolt that travelled
	 * thirty metres wet the ground where it landed, not the whole line of flight.
	 * So this is off by default and the disc is what a projectile or a burst gets.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Magic|Deposit")
	bool bDepositSwept = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGHitboxComponent> Hitbox;

	/**
	 * Raised when a spell with a deposit radius finishes, so a body of its element
	 * can be left where it ended.
	 *
	 * WHY A STATIC DELEGATE RATHER THAN A DIRECT CALL, and it is the same reason
	 * UARPGElementalVolumeComponent::OnVolumesMet is one: the fluid system lives in
	 * ARPGWorld, which already depends on ARPGMagic, so an effect calling it
	 * directly would close the cycle. Inverting it here keeps the dependency
	 * running one way and keeps a discharge effect a description of a cast spell,
	 * with no knowledge of what the ground does with what it spills.
	 *
	 * Subscribers MUST filter by world: this is process-wide, so a PIE session with
	 * a server and a client world would otherwise cross-talk.
	 */
	static FARPGOnDischargeLanded OnDischargeLanded;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Magic")
	FARPGDischargeContext Context;
};
