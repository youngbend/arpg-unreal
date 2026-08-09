// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "UObject/Interface.h"
#include "ARPGMagicProgression.generated.h"

UINTERFACE(MinimalAPI, BlueprintType)
class UARPGMagicProgression : public UInterface
{
	GENERATED_BODY()
};

/**
 * What the magic system needs to know about a caster's mastery, and nothing more.
 *
 * WHY AN INTERFACE RATHER THAN A DIRECT DEPENDENCY. The progression tracker
 * itself is phase 7, but complexity gating and mastery scaling are phase 5 --
 * they are part of how casting WORKS, not part of how it is earned. Narrowing
 * the coupling to these two questions means phase 5 is complete and testable
 * now, and phase 7 fills it in by implementing two functions.
 *
 * A caster with no implementation is not an error: gating is disabled and the
 * damage multiplier is 1, which is exactly right for an NPC that should cast
 * without a progression system behind it. That was the Godot behaviour too --
 * a null tracker meant "no gating" rather than "no casting".
 */
class ARPGMAGIC_API IARPGMagicProgression
{
	GENERATED_BODY()

public:
	/**
	 * Mastery level with this element, as a float so partial progress counts.
	 * The complexity gate compares it against other elements' complexity.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "ARPG|Magic")
	float GetEffectiveElementLevel(FGameplayTag ElementTag) const;

	/** Multiplier on discharge damage for this element. 1 means no bonus. */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "ARPG|Magic")
	float GetElementDamageMultiplier(FGameplayTag ElementTag) const;
};
