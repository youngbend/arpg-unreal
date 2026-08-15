// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "ARPGFuelComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnFuelSpent, UARPGFuelComponent*, Fuel);

/**
 * An object that is FUEL: it feeds a spreading medium, and is consumed doing it.
 *
 * THE GROUND ALREADY BURNS AND OBJECTS DID NOT. The spread field carries fuel per
 * cell, baked from the world, and a wooden crate standing on bare stone was
 * scenery the fire went round. This is the other half: a thing that adds to the
 * fire where it stands and has a finite amount of itself to add.
 *
 * FUEL IS NOT HEALTH, and keeping them apart is the whole design. Health is what
 * the fire TAKES from the object -- the hurtbox and TickContactDamage, which
 * already work. Fuel is what the object GIVES to the fire. A powder keg is enormous
 * fuel and no health; a stone brazier is no fuel and plenty of health; an oak
 * beam is both. Collapsing them into one number makes all three the same thing.
 *
 * AND IT BURNS PARTWAY. Fuel is spent only while the cells under the object are
 * actually alight, so water that quenches the field stops the burn where it
 * stands -- a crate half consumed, still standing, still fuel. Relight it and it
 * burns the remainder. Nothing implements that: it is what "spend only while
 * alight" means, and the half-burnt crate falls out of it.
 *
 * THE OBJECT KEEPS ITS OWN FUEL rather than pushing it into the field. The field
 * has fuel of its own in the same cells -- the ground -- and once the two are
 * mixed there is no telling whose is whose, so quenching would refund the crate
 * and burning the grass would consume it. Here, the field says only whether the
 * cell is alight, and the object answers with what it is prepared to give.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGWORLD_API UARPGFuelComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGFuelComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * What this is fuel FOR, as a medium tag.
	 *
	 * PER MEDIUM AND NOT PER DAMAGE TYPE, which is the distinction that makes this
	 * worth a component rather than a negative resistance. Steam does fire damage
	 * and is not a fire medium; a thing weak to fire is not thereby kindling. What
	 * an object burns AS is a separate fact from what hurts it, and this is it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|Fuel",
		meta = (Categories = "Element"))
	FGameplayTag MediumTag;

	/** Seconds of burning this can sustain from whole. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|Fuel",
		meta = (ClampMin = "0.0"))
	float FuelSeconds = 20.f;

	/**
	 * How much of that is left, in seconds.
	 *
	 * REPLICATED, because a half-burnt crate is a visible state and not an
	 * accounting detail: clients read the char off it to darken the mesh, and a
	 * client that thought a spent crate was whole would show a pristine box
	 * standing in the ashes of the fire that ate it.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Fuel, BlueprintReadOnly, Category = "ARPG|Fuel")
	float FuelRemaining = -1.f;

	/**
	 * How far the object feeds the field, in cm.
	 *
	 * Its own footprint, roughly. Generous is fine and mean is not: an object
	 * feeding a smaller radius than it occupies leaves a fire that burns around
	 * its edges and never quite catches it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|Fuel",
		meta = (ClampMin = "1.0"))
	float Radius = 80.f;

	/**
	 * Intensity per second it feeds into its cells while alight.
	 *
	 * WHAT MAKES IT WORTH BURNING. Without this an object is only a thing that
	 * gets consumed; with it, setting a woodpile alight is a way to make a bigger
	 * fire than the grass around it could sustain, which is the reason to care
	 * where the flammable things are.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|Fuel",
		meta = (ClampMin = "0.0"))
	float Output = 1.5f;

	/**
	 * Intensity below which the object does not consider itself alight.
	 *
	 * A fire licking at something is not a thing burning. Without a floor an
	 * object in the dying embers of a grass fire is consumed a hundredth at a
	 * time until nothing is left, which reads as decay rather than as burning.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|Fuel",
		meta = (ClampMin = "0.0"))
	float CatchThreshold = 0.15f;

	/**
	 * Scalar parameter index for the char value, in custom primitive data.
	 *
	 * NOT A MATERIAL INSTANCE PER OBJECT. A dynamic instance per crate breaks
	 * batching and allocates, and a level of them is a real cost for one float.
	 * Custom primitive data is the cheap way to give every instance its own
	 * number while they all share one material. Negative disables it, for an
	 * actor that would rather listen to the delegate and swap a mesh.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|Fuel")
	int32 CharParameterIndex = 0;

	/** 0 whole, 1 spent. What the char looks like. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fuel")
	float GetCharred() const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Fuel")
	bool IsSpent() const { return FuelRemaining <= 0.f; }

	/** True while the cells under it were alight on the last tick. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fuel")
	bool IsAlight() const { return bAlight; }

	/**
	 * Nothing left to give.
	 *
	 * THE ACTOR DECIDES WHAT THAT MEANS, because it differs and this cannot know:
	 * a log becomes charcoal, a rope parts, a barricade collapses, a crate spills
	 * what was in it. Spent is not destroyed.
	 */
	UPROPERTY(BlueprintAssignable, Category = "ARPG|Fuel")
	FARPGOnFuelSpent OnSpent;

	/**
	 * Advances the burn by one step of the spread tick.
	 *
	 * Called by the subsystem, which is the only thing that knows whether the
	 * cells under this are alight.
	 *
	 * @return intensity to feed back into the field, or 0 when it is not burning.
	 */
	float Burn(float DeltaTime, float Intensity);

	/** Puts fuel back. For a repaired barricade, or an editor reset. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fuel")
	void Replenish(float Seconds);

protected:
	UFUNCTION()
	void OnRep_Fuel();

	/** Pushes the char onto the owner's primitives, wherever it is being read. */
	void PublishChar();

	UPROPERTY(Transient)
	bool bAlight = false;
};
