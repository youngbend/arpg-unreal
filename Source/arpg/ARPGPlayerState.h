// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "AbilitySystemInterface.h"
#include "ARPGPlayerState.generated.h"

class UARPGAbilitySystemComponent;

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

private:
	UPROPERTY(VisibleAnywhere, Category = "ARPG|Abilities")
	TObjectPtr<UARPGAbilitySystemComponent> AbilitySystemComponent;
};
