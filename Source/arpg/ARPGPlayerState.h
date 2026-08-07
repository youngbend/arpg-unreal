// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "AbilitySystemInterface.h"
#include "ARPGPlayerState.generated.h"

class UARPGAbilitySystemComponent;
class UARPGVitalSet;
class UARPGOffenseSet;
class UARPGResistanceSet;

/**
 * Owns the player's ability system component.
 *
 * WHY THE PLAYER STATE AND NOT THE PAWN. A PlayerState survives pawn death and
 * respawn, and the player's attributes, cooldowns and progression have to
 * survive with it -- putting the ASC on the pawn means rebuilding all of that
 * every time the player dies. NPCs do the opposite (ASC on the pawn) because
 * their state is meant to die with them.
 */
UCLASS()
class AARPGPlayerState : public APlayerState, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AARPGPlayerState();

	//~ IAbilitySystemInterface
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	//~ End IAbilitySystemInterface

	/** Typed accessor, so call sites don't cast. */
	UARPGAbilitySystemComponent* GetARPGAbilitySystemComponent() const
	{
		return AbilitySystemComponent;
	}

	UARPGVitalSet*      GetVitalSet() const      { return VitalSet; }
	UARPGOffenseSet*    GetOffenseSet() const    { return OffenseSet; }
	UARPGResistanceSet* GetResistanceSet() const { return ResistanceSet; }

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY(VisibleAnywhere, Category = "ARPG|Abilities")
	TObjectPtr<UARPGAbilitySystemComponent> AbilitySystemComponent;

	// Attribute sets are subobjects of the ASC's OWNING ACTOR, which is how GAS
	// discovers and registers them. Creating them anywhere else (on the pawn, at
	// runtime) means they never get registered and every attribute reads as 0.
	UPROPERTY()
	TObjectPtr<UARPGVitalSet> VitalSet;

	UPROPERTY()
	TObjectPtr<UARPGOffenseSet> OffenseSet;

	UPROPERTY()
	TObjectPtr<UARPGResistanceSet> ResistanceSet;
};
