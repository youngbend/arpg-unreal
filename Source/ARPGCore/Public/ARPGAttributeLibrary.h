// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AttributeSet.h"
#include "ARPGAttributeLibrary.generated.h"

class UAbilitySystemComponent;

/**
 * Correct read-modify-write against a gameplay attribute, in one place.
 *
 * THE BUG THIS EXISTS TO PREVENT. UAbilitySystemComponent has two readers and
 * they are not interchangeable:
 *
 *   GetNumericAttribute      the CURRENT value -- base plus every active
 *                            GameplayEffect modifier
 *   GetNumericAttributeBase  the BASE value alone
 *
 * and one writer, SetNumericAttributeBase, which writes the base. Reading the
 * current value and writing it back as the base folds every active modifier
 * permanently into the base:
 *
 *   base 0, a Wet status adding +0.1 fire resistance  -> current 0.1
 *   armour adds 0.2, read current (0.1), write base   -> base 0.3, current 0.4
 *   Wet expires                                       -> base stays 0.3
 *
 * Every spend and every equipment grant in this project had that shape. Routing
 * them through here makes the pairing impossible to get wrong, because the
 * caller never names a reader at all.
 *
 * These are for state a COMPONENT owns outright. A cost an ABILITY owns belongs
 * on that ability's CostGameplayEffectClass instead, which additionally gets
 * client prediction -- see UARPGGameplayAbility_Discharge.
 */
UCLASS()
class ARPGCORE_API UARPGAttributeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Spends from an attribute's base value, or returns false having spent
	 * nothing.
	 *
	 * Affordability is tested against the CURRENT value -- a buff that grants
	 * temporary stamina should be spendable -- while the write lands on the base,
	 * which is where a spend belongs. That asymmetry is deliberate and is the one
	 * place the two readers legitimately differ.
	 *
	 * Returns TRUE when there is no ability system: a character with no
	 * attributes is one the resource system does not apply to, not one that can
	 * never act. Every caller in the project relies on this.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Attributes")
	static bool TrySpend(UAbilitySystemComponent* ASC, const FGameplayAttribute& Attribute, float Cost);

	/**
	 * Adds to an attribute's base value, clamped to >= Min. Negative adds
	 * subtract.
	 *
	 * Min is not defaulted: UHT cannot parse an expression default, and a silent
	 * floor is the kind of thing a caller should have to state anyway. Pass
	 * -MAX_flt for an unclamped add.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Attributes")
	static void AddToBase(UAbilitySystemComponent* ASC, const FGameplayAttribute& Attribute,
		float Delta, float Min);

	/** Sets an attribute's base value outright. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Attributes")
	static void SetBase(UAbilitySystemComponent* ASC, const FGameplayAttribute& Attribute, float Value);

	/** The base value, or 0 with no ability system. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Attributes")
	static float GetBase(const UAbilitySystemComponent* ASC, const FGameplayAttribute& Attribute);

	/** The current value (base + modifiers), or 0 with no ability system. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Attributes")
	static float GetCurrent(const UAbilitySystemComponent* ASC, const FGameplayAttribute& Attribute);
};
