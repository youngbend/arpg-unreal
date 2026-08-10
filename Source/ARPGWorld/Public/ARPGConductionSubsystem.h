// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Subsystems/WorldSubsystem.h"
#include "ARPGConductionSubsystem.generated.h"

class AActor;
class UARPGElementalVolumeComponent;
class UARPGMagicCombinationTable;
class UARPGMagicElement;

/**
 * The THIRD solver, and the one the other two could not express: an element
 * entering a medium and travelling through everything that medium touches, all
 * in a single frame. Port of Godot's ConductionSystem.
 *
 *   COLLISION   (reaction subsystem)  pooled energies, exchanged once on
 *                                     overlap. Two things meet and trade.
 *   DIFFUSION   (spread subsystem)    per-cell intensity creeping outward over
 *                                     many ticks.
 *   CONDUCTION  (this)                one charge, one frame, following a
 *                                     connected medium as far as its energy
 *                                     lasts.
 *
 * Lightning into a puddle should not "react with" the water and it should not
 * creep. It should arrive, run through every puddle that puddle touches, hurt
 * whatever is standing in them, and be over.
 *
 * NOTHING HERE KNOWS WHAT LIGHTNING OR WATER ARE. A conduction relationship is
 * one Conduct row in the shared combination table naming what travels and what
 * carries it; how well a given piece of world carries it is the volume's own
 * Conductivity. So lightning-through-water and light-through-glass are the same
 * feature twice, with no code between them. A run of stained-glass windows
 * carries a light spell along the whole run, and one with a pane missing does
 * not.
 *
 * THE GRAPH IS FREE. "Which media touch each other" is already answered by the
 * physics overlaps a volume has. A chain of puddles that overlap conducts; two
 * puddles with dry ground between them do not, and nobody authored either fact.
 * Non-conductive volumes -- every projectile -- drop out by the same filter.
 */
UCLASS()
class ARPGWORLD_API UARPGConductionSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	/** The SAME table every other solver reads. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Conduction")
	TObjectPtr<UARPGMagicCombinationTable> CombinationTable;

	/**
	 * Energy lost per centimetre travelled, as a fraction. Hops cost
	 * conductivity; this is what stops one enormous lake conducting a spark to
	 * its far shore.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Conduction",
		meta = (ClampMin = "0.0"))
	float DistanceLoss = 0.0005f;

	/** Charges weaker than this stop propagating and deal no damage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Conduction",
		meta = (ClampMin = "0.0"))
	float MinEnergy = 1.f;

	/** Safety rail on a pathological chain of overlapping volumes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Conduction",
		meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxHops = 16;

	/** Multiplies arriving energy into the damage each caught target takes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Conduction",
		meta = (ClampMin = "0.0"))
	float DamageScale = 1.f;

	/**
	 * Floods the charge through the entry medium and everything it touches.
	 * Returns how many media were reached.
	 *
	 * Called by the reaction subsystem when a Conduct row matches, and safe to
	 * call directly from a trap or a test.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Conduction")
	int32 Conduct(UARPGMagicElement* ChargeElement, UARPGElementalVolumeComponent* EntryMedium,
		float Energy, AActor* SourceActor);

	/** Media reached by the last flood -- what a test asserts the chain on. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Conduction")
	int32 GetLastReachedCount() const { return LastReachedCount; }

	/** Total damage the last flood delivered, across every medium and target. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Conduction")
	float GetLastDeliveredDamage() const { return LastDeliveredDamage; }

private:
	/** Does a Conduct row say this charge travels through this medium? */
	bool Conducts(const FGameplayTag& ChargeTag, const FGameplayTag& MediumTag) const;

	/** True where this machine owns the simulation. Conduction deals damage. */
	bool HasAuthority() const;

	/**
	 * The combination table, resolved from project settings on first use.
	 *
	 * The CombinationTable field above is EditAnywhere on a UWorldSubsystem,
	 * which has no editing surface at all -- so outside the tests it was always
	 * null and no charge ever conducted anywhere.
	 */
	const UARPGMagicCombinationTable* GetTable() const;

	/** Damages everything genuinely standing in the medium. */
	void StrikeTargets(UARPGElementalVolumeComponent* Medium, UARPGMagicElement* ChargeElement,
		float Energy, AActor* SourceActor);

	int32 LastReachedCount = 0;
	float LastDeliveredDamage = 0.f;
};
