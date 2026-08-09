// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "ARPGMagicTypes.h"
#include "ARPGMagicCombinationTable.generated.h"

class UARPGMagicElement;

/**
 * One row in the combination table -- the single place any relationship between
 * elements is authored, whatever consumes it. Port of Godot's
 * MagicCombinationEntry.
 *
 * THREE CONSUMERS, ONE TABLE, BUT NOT ONE RULE SET:
 *
 *   Magic component     the player holding two elements at once. Reads
 *                       RequiredElements and Result only.
 *   Reaction system     two spells colliding. Also reads the consumption
 *                       rates, pooled.
 *   Spread system       two media sharing ground -- rain against a grass
 *                       fire. Same rates, applied per tick.
 *
 * Some relationships mean the same thing everywhere and are worth authoring
 * once. Plenty do not, which is what Scope is for -- see EARPGCombinationScope.
 *
 * AMPLIFICATION needs no separate concept: when Result IS one of the reactants,
 * that is one element feeding the other rather than a transmutation. Fire + air
 * -> fire makes a fireball eat an air projectile and grow, and makes wind fan a
 * grass fire, from the same row.
 */
UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced)
class ARPGMAGIC_API UARPGMagicCombinationEntry : public UObject
{
	GENERATED_BODY()

public:
	/** The exact set of element tags that must be active. Order is irrelevant. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recipe",
		meta = (Categories = "Element"))
	FGameplayTagContainer RequiredElements;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recipe")
	TObjectPtr<UARPGMagicElement> Result;

	/**
	 * Defaults to everything, which is a convenience rather than an assertion:
	 * it says "nobody has thought about where this applies yet", not "this is
	 * true everywhere". Narrow anything context-dependent.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recipe",
		meta = (Bitmask, BitmaskEnum = "/Script/ARPGMagic.EARPGCombinationScope"))
	int32 Scope = 0xF;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recipe")
	EARPGReactionMode Mode = EARPGReactionMode::Auto;

	/** Highest priority wins when several rows could match. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recipe")
	int32 Priority = 0;

	/**
	 * How fast each reactant is consumed. Anything unlisted is 1, so a plain
	 * recipe is a straight 1:1 trade and only lopsided ones need authoring.
	 *
	 * Asymmetry is what makes a relationship directional: water quenches fire
	 * far faster than fire boils off water, so the burning side carries a high
	 * rate and the wet side a low one. The same numbers make a water jet punch
	 * through a fireball rather than trading evenly with it.
	 *
	 * Keyed by tag rather than a second array parallel to RequiredElements --
	 * that pairing was by index in the Godot version, which silently mis-assigns
	 * every rate the moment a reactant is inserted rather than appended.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Reaction",
		meta = (Categories = "Element"))
	TMap<FGameplayTag, float> ConsumptionRates;

	/**
	 * Fraction of the consumed element's energy the survivor gains when this row
	 * amplifies. Below 1 so a chain of amplifications converges instead of
	 * snowballing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Reaction",
		meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float AmplificationEfficiency = 0.6f;

	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	bool AppliesTo(EARPGCombinationScope InScope) const
	{
		return (Scope & static_cast<int32>(InScope)) != 0;
	}

	/** True when the active set is exactly this row's reactants. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	bool Matches(const FGameplayTagContainer& ActiveElements) const;

	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	float GetConsumptionRate(FGameplayTag ElementTag) const;

	/**
	 * True when Result is one of the reactants and this is not conduction.
	 *
	 * The conduction exclusion matters: a conduction row also has Result equal
	 * to a reactant, but it means something entirely different, so it must not
	 * fall through to amplification.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	bool IsAmplifying() const;

	/** The tag this row amplifies, or an empty tag when it is a plain combination. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	FGameplayTag GetAmplifiedElement() const;

	/** For a conduction row: the element that travels. Empty otherwise. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	FGameplayTag GetConductedElement() const;

	/** For a conduction row: the medium carrying it. Empty otherwise. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	FGameplayTag GetConductorElement() const;
};

/**
 * Every relationship between elements, in one asset. Port of Godot's
 * MagicCombinationTable.
 *
 * Resolution is always scoped -- there is no unscoped overload on purpose. A row
 * can mean one thing in a caster's hands and something else in the world, so
 * every consumer has to say which rule set it is asking about.
 */
UCLASS(BlueprintType)
class ARPGMAGIC_API UARPGMagicCombinationTable : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Instanced, Category = "Combinations")
	TArray<TObjectPtr<UARPGMagicCombinationEntry>> Entries;

	/** The result of the best-matching row in this scope, or null. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	UARPGMagicElement* Resolve(const FGameplayTagContainer& ActiveElements,
		EARPGCombinationScope InScope) const;

	/**
	 * The winning ROW rather than just its result, for consumers that need the
	 * relationship's rates and amplification efficiency as well -- the reaction
	 * and spread systems both do.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	UARPGMagicCombinationEntry* ResolveEntry(const FGameplayTagContainer& ActiveElements,
		EARPGCombinationScope InScope) const;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGCombinationTable", GetFName());
	}

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
};
