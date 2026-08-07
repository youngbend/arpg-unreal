// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "ARPGStatusResistanceComponent.generated.h"

/**
 * Target-side resistance to status effects. Port of CombatStats'
 * status_resistances dictionary and immunities array.
 *
 * WHY NOT ATTRIBUTES. Per-damage-type resistance became one attribute per type
 * (see UARPGResistanceSet) because there are seven of them and each needs to be
 * buffable by a duration effect. Status effects are open-ended -- every new
 * status would mean a new attribute -- and what is stored is a probability
 * consulted once at application, not a value modified over time. A tag-keyed map
 * is the honest shape for that.
 *
 * Timed status immunity is deliberately NOT here: that is what GAS's
 * GrantedApplicationImmunityTags already does, granted by whatever effect
 * confers the immunity. This holds only what an archetype permanently is.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCOMBAT_API UARPGStatusResistanceComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGStatusResistanceComponent();

	/**
	 * Status tag -> probability [0,1] of resisting application outright.
	 * 1.0 is equivalent to immunity; prefer Immunities for that, so intent reads.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Status",
		meta = (Categories = "Status"))
	TMap<FGameplayTag, float> StatusResistances;

	/** Statuses this archetype can never be afflicted by, at any probability. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Status",
		meta = (Categories = "Status"))
	FGameplayTagContainer Immunities;

	UFUNCTION(BlueprintPure, Category = "ARPG|Status")
	bool IsImmuneTo(FGameplayTag StatusTag) const
	{
		return StatusTag.IsValid() && Immunities.HasTagExact(StatusTag);
	}

	UFUNCTION(BlueprintPure, Category = "ARPG|Status")
	float GetResistance(FGameplayTag StatusTag) const
	{
		const float* Found = StatusResistances.Find(StatusTag);
		return Found ? FMath::Clamp(*Found, 0.f, 1.f) : 0.f;
	}
};
