// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "ARPGAttributeSetBase.generated.h"

/**
 * Generates the four accessors GAS expects for an attribute: the static
 * FGameplayAttribute getter, plus value get/set/init. Without these, every
 * capture and every SetByCaller site has to spell out the property by hand.
 */
#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

/**
 * Common base for the project's attribute sets.
 *
 * Exists mainly to hold the shared OnRep helper -- every replicated attribute
 * needs to notify GAS on arrival or the client's aggregated value silently
 * drifts from the server's.
 */
UCLASS()
class ARPGCORE_API UARPGAttributeSetBase : public UAttributeSet
{
	GENERATED_BODY()

public:
	/** Convenience: the ASC that owns this set, or null. */
	UARPGAttributeSetBase() = default;
};

/**
 * Boilerplate every replicated attribute's OnRep needs. GAMEPLAYATTRIBUTE_REPNOTIFY
 * tells the ability system an attribute arrived from the network so it can rebase
 * its aggregators; skipping it is the classic cause of "the client's health bar is
 * right until a buff is applied, then permanently wrong".
 */
#define ARPG_REPNOTIFY(ClassName, PropertyName, OldValue) \
	GAMEPLAYATTRIBUTE_REPNOTIFY(ClassName, PropertyName, OldValue)
