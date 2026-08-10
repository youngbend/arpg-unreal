// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ARPGGameplayComponentBase.generated.h"

class UAbilitySystemComponent;

/**
 * Shared base for every ARPG component that talks to the ability system.
 *
 * Exists for one reason: eight components had copied the same lazily-cached
 * GetASC() out of each other, and two more had an uncached variant that did the
 * global lookup on every call. One copy means the caching rule -- and the reason
 * for it -- lives in one place.
 *
 * WHY LAZY RATHER THAN BEGINPLAY. A player's ability system lives on the
 * PlayerState, which on a client may not have replicated in by the time this
 * component begins play. Resolving on first use and retrying while null is the
 * only version that works for both a player pawn and an NPC that owns its own
 * component outright.
 */
UCLASS(Abstract)
class ARPGCORE_API UARPGGameplayComponentBase : public UActorComponent
{
	GENERATED_BODY()

public:
	/** The owner's ability system component, resolved on first use. May be null. */
	UFUNCTION(BlueprintPure, Category = "ARPG")
	UAbilitySystemComponent* GetASC() const;

	/** True when this component may mutate authoritative gameplay state. */
	UFUNCTION(BlueprintPure, Category = "ARPG")
	bool HasAuthority() const;

protected:
	/**
	 * Drops the cached pointer, so the next GetASC() resolves again.
	 *
	 * Needed on a pawn that is re-possessed: the ability system moves with the
	 * PlayerState, and a stale pointer would keep writing to the previous
	 * player's attributes.
	 */
	void InvalidateASC() const { CachedASC = nullptr; }

private:
	UPROPERTY(Transient)
	mutable TObjectPtr<UAbilitySystemComponent> CachedASC;
};
