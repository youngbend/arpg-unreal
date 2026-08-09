// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Subsystems/WorldSubsystem.h"
#include "ARPGElementalReactionSubsystem.generated.h"

class UARPGElementalVolumeComponent;
class UARPGMagicCombinationTable;
class UARPGMagicElement;

/**
 * Resolves what happens where two elemental volumes meet: a fireball into a
 * water jet, a fireball into a river, an ice shard into a lava flow. Port of
 * Godot's ElementalReactionSystem.
 *
 * THE MODEL. Both sides carry energy in discharge-damage units, so spell power
 * feeds in already scaled by charge, mastery and buffs:
 *
 *   Reacted    = min(EnergyA, EnergyB)              energy actually converted
 *   Absorption = max(AbsorptionA, AbsorptionB)      reservoirs damp the release
 *   Magnitude  = Reacted * (1 - Absorption)         size of the product burst
 *   Surplus    = |EnergyA - EnergyB|                what survives, and whose
 *
 * which gives the three cases that matter with NO case analysis anywhere:
 *
 *   Two matched projectiles    Reacted = e, both spent, full burst.
 *   Big fire vs small water    Burst sized to the WATER; the fire carries on at
 *                              (surplus / energy) power.
 *   Anything vs a river        Reacted = the projectile, since a river cannot be
 *                              depleted -- but absorption is high, so the release
 *                              is a hiss rather than a blast.
 *
 * THE PRODUCT is whatever the combination table resolves for the two elements --
 * the same table and the same recipes that govern a player combining elements in
 * hand, so fire+water is steam in both places and there is no second rule set to
 * keep in sync. It spawns as a COLLISION discharge, which is its own type for a
 * real reason: a burst is a cone the caster aims, a collision releases outward
 * from wherever the two volumes actually touched.
 *
 * Two elements with no recipe still NEUTRALISE (energy is exchanged, both sides
 * weaken) but produce nothing visible. Two volumes of the same element never
 * react -- two fireballs meeting should merge or pass, not annihilate.
 */
UCLASS()
class ARPGWORLD_API UARPGElementalReactionSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	/** Every solver reads the SAME table the player's magic component does. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Reactions")
	TObjectPtr<UARPGMagicCombinationTable> CombinationTable;

	/** Multiplies reaction magnitude into the product's damage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Reactions",
		meta = (ClampMin = "0.0"))
	float DamageScale = 1.f;

	/**
	 * Magnitude that reads as full power for the product's visuals. Sets how big
	 * a reaction has to be before its burst stops growing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Reactions",
		meta = (ClampMin = "0.01"))
	float PowerReference = 40.f;

	/**
	 * Reactions below this produce no burst at all, so a spent ember touching a
	 * lake does not pop a full steam cloud.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Reactions",
		meta = (ClampMin = "0.0"))
	float MinMagnitude = 1.f;

	/** Two elements with no recipe still cancel out, just silently. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Reactions")
	bool bNeutraliseWithoutProduct = true;

	/**
	 * Resolves one meeting. Safe to call directly -- a scripted hazard, a test --
	 * as well as from the overlap that normally triggers it.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Reactions")
	void Resolve(UARPGElementalVolumeComponent* A, UARPGElementalVolumeComponent* B);

	/**
	 * Where the last reaction was placed.
	 *
	 * Exposed because "the steam appeared in the wrong place" is a fault with no
	 * other symptom -- it does not throw, log, or change any number.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Reactions")
	FVector GetLastContactPoint() const { return LastContactPoint; }

	/** What the last reaction produced, or null if it merely neutralised. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Reactions")
	UARPGMagicElement* GetLastProduct() const { return LastProduct; }

private:
	void HandleVolumesMet(UARPGElementalVolumeComponent* A, UARPGElementalVolumeComponent* B);

	/** Spawns the product as a Collision discharge at the contact point. */
	void SpawnProduct(UARPGMagicElement* Product, const FVector& Contact,
		const FVector& Direction, float Magnitude, AActor* SourceActor);

	FVector LastContactPoint = FVector::ZeroVector;

	UPROPERTY(Transient)
	TObjectPtr<UARPGMagicElement> LastProduct;

	FDelegateHandle VolumesMetHandle;
};
