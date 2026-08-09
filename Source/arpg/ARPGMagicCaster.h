// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGCombatDummy.h"
#include "ARPGMagicProgression.h"
#include "ARPGMagicCaster.generated.h"

class UARPGCloakComponent;
class UARPGMagicComponent;

/**
 * A dummy that can cast, so the magic pipeline is observable before there is a
 * player character wired to magic input. The phase 5 counterpart to
 * AARPGCombatDummy, and it inherits from it for the same reason: a caster is
 * something you also want to be able to hit.
 *
 * IT SUPPLIES ITS OWN MASTERY. Implementing IARPGMagicProgression with a plain
 * authored map is what makes complexity gating testable now rather than in phase
 * 7 -- the gate needs an answer to "how good is this caster with fire", not the
 * whole progression system that eventually computes it. Leave ElementLevels
 * empty and every element reads as level 0, which gates out any combination of
 * complexity 1 or higher.
 */
UCLASS()
class AARPGMagicCaster : public AARPGCombatDummy, public IARPGMagicProgression
{
	GENERATED_BODY()

public:
	AARPGMagicCaster();

	/**
	 * Mastery per element. Unlisted elements are level 0.
	 *
	 * The gate compares this against other elements' Complexity, so a caster
	 * needs level >= 2 in both fire and water before they may hold a
	 * complexity-2 pair.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Caster",
		meta = (Categories = "Element"))
	TMap<FGameplayTag, float> ElementLevels;

	/** Damage multiplier per element. Unlisted elements are 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Caster",
		meta = (Categories = "Element"))
	TMap<FGameplayTag, float> ElementDamageMultipliers;

	virtual float GetEffectiveElementLevel_Implementation(FGameplayTag ElementTag) const override;
	virtual float GetElementDamageMultiplier_Implementation(FGameplayTag ElementTag) const override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGMagicComponent> Magic;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGCloakComponent> Cloak;
};
