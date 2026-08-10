// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "AttributeSet.h"
#include "GameplayTagContainer.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ARPGEquipmentEffect.generated.h"

class UAbilitySystemComponent;

/**
 * The infinite-duration effect a worn or wielded piece grants while it is worn.
 *
 * WHY AN EFFECT RATHER THAN WRITING THE ATTRIBUTE. Both equipment components
 * used to add their contribution straight onto the attribute's base value and
 * remember the amount so they could subtract it again. Two things went wrong
 * with that, and neither is fixable by being more careful:
 *
 *   The read and the write disagreed. Reading the CURRENT value (base plus every
 *   active modifier) and writing it back as the BASE folded any live buff into
 *   the base permanently -- so a fire-resist piece equipped while Wet kept the
 *   Wet bonus forever. See UARPGAttributeLibrary.
 *
 *   Exact reversal is not actually possible by hand. The remembered amount is
 *   only correct if nothing else touched the attribute in between, which is
 *   precisely what an aggregator exists to allow.
 *
 * As a modifier the aggregator owns the contribution for the effect's lifetime,
 * removal is exact by construction, and the value composes with buffs, statuses
 * and archetype instead of racing them.
 *
 * WHY SETBYCALLER RATHER THAN A GE BUILT AT RUNTIME. A UGameplayEffect created
 * with NewObject has no network identity -- the client cannot resolve the
 * definition pointer, so the owning player's own equipment arrives with a null
 * Def under Mixed replication. A fixed class with one SetByCaller modifier per
 * attribute replicates like any other effect. The cost is that adding a
 * resistance attribute means a line here, which is the same static-capture trade
 * UARPGDamageExecution already makes and for the same reason.
 *
 * Modifiers whose caller magnitude is zero are harmless, so a piece that grants
 * only armour still applies the whole set.
 */
UCLASS()
class ARPGCOMBAT_API UARPGEquipmentGameplayEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UARPGEquipmentGameplayEffect();
};

/** Applying and removing an equipment grant, so both components do it identically. */
UCLASS()
class ARPGCOMBAT_API UARPGEquipmentEffectLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Removes any previous grant and applies a new one.
	 *
	 * @param Grants  attribute -> additive amount. Attributes the equipment
	 *                effect does not carry a modifier for are reported and
	 *                skipped: silently dropping one makes a piece look like it
	 *                simply does not work.
	 * @param InOutHandle  updated in place; invalidated when Grants is empty.
	 */
	static void ApplyGrant(UAbilitySystemComponent* ASC,
		const TArray<TPair<FGameplayAttribute, float>>& Grants,
		FActiveGameplayEffectHandle& InOutHandle);

	/** Removes a grant if one is live, and clears the handle. */
	static void RemoveGrant(UAbilitySystemComponent* ASC, FActiveGameplayEffectHandle& InOutHandle);

	/**
	 * The SetByCaller tag that drives an attribute's modifier, or an invalid tag
	 * when the equipment effect carries no modifier for it.
	 */
	static FGameplayTag FindSetByCallerTag(const FGameplayAttribute& Attribute);
};
