// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectKey.h"
#include "ARPGHurtboxRegistry.generated.h"

class AActor;
class UARPGHurtboxComponent;

/**
 * Actor -> hurtbox, so "is this thing damageable" is a map lookup.
 *
 * WHY THIS IS NOT A COLLISION CHANNEL. The obvious alternative is to give
 * hurtboxes their own object type so a hitbox sweep only ever returns damageable
 * things. That would undo the deliberate decision documented on
 * UARPGHurtboxComponent: the hurtbox is NOT a collision volume, because traces
 * already hit the skeletal mesh's physics asset and a second coarser volume
 * would throw away the per-bone information that gives us. A registry keeps that
 * property and still removes the per-candidate component walk.
 *
 * PER WORLD, not a static map. A process-wide one would let a PIE server's
 * actors answer a PIE client's queries -- the exact hazard the static
 * OnVolumesMet delegate has to guard against by hand.
 *
 * Weak pointers plus explicit unregistration on EndPlay: an actor torn down
 * without EndPlay (a world teardown) leaves a stale entry, which the lookup
 * drops when it resolves to null.
 */
UCLASS()
class ARPGCOMBAT_API UARPGHurtboxRegistry : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	void Register(AActor* Actor, UARPGHurtboxComponent* Hurtbox);
	void Unregister(const AActor* Actor);

	/** Null when the actor is not damageable, or its entry has gone stale. */
	UARPGHurtboxComponent* Find(const AActor* Actor);

private:
	TMap<FObjectKey, TWeakObjectPtr<UARPGHurtboxComponent>> Hurtboxes;
};
