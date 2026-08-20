// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "ARPGNPCCharacter.generated.h"

class UARPGAbilitySystemComponent;
class UARPGComboComponent;
class UARPGHitboxComponent;
class UARPGHurtboxComponent;
class UARPGInventoryComponent;
class UARPGNPCComponent;
class UARPGNoiseComponent;
class UARPGOffenseSet;
class UARPGParryComponent;
class UARPGPoiseComponent;
class UARPGQuickSlotComponent;
class UARPGResistanceSet;
class UARPGVitalSet;
class UARPGWeaponComponent;

/**
 * The body every NPC uses. One class for every enemy in the game.
 *
 * THERE IS DELIBERATELY NO SUBCLASS PER ENEMY. What a goblin IS lives in its
 * UARPGNPCDefinition -- faction, stats, weapon, mesh, tree -- and this assembles
 * only the machinery that every NPC needs regardless of archetype. Adding an
 * enemy type is an asset, never a class and never a Blueprint hierarchy. See
 * UARPGNPCComponent for the other half of that argument.
 *
 * THE ASC LIVES HERE, NOT ON A PLAYER STATE, and replicates Minimal: nobody owns
 * an NPC, so no client needs its full gameplay-effect list -- replicated tags
 * and attributes are enough. That is the split §3.1 of the port plan calls for,
 * and it is why this implements IAbilitySystemInterface directly rather than
 * forwarding the way AarpgCharacter does.
 *
 * WHAT IT DOES NOT HAVE, on purpose: no camera, no input, no locomotion tiers,
 * no magic. Those are the player's, and an NPC that carried them would be
 * carrying components nothing ever drives.
 */
UCLASS()
class AARPGNPCCharacter : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AARPGNPCCharacter();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

	UFUNCTION(BlueprintPure, Category = "ARPG|NPC")
	UARPGNPCComponent* GetNPCComponent() const { return NPCComponent; }

protected:
	virtual void BeginPlay() override;

	/**
	 * The archetype. Assigned on the NPC component rather than duplicated here,
	 * so there is exactly one place an instance says what it is.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGNPCComponent> NPCComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGAbilitySystemComponent> AbilitySystemComponent;

	/** Takes hits and, through the NPC component, turns them into knowledge. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGHurtboxComponent> Hurtbox;

	/**
	 * The swung weapon, on the right-hand socket.
	 *
	 * ON A SOCKET rather than parked in front of the actor the way the player's
	 * debug hitbox is, because this one is read as GEOMETRY: the reactive parry
	 * tracks its live position to solve for time to contact, so it has to
	 * actually ride the animated hand or the prediction is measuring nothing.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGHitboxComponent> WeaponHitbox;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGComboComponent> ComboComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGWeaponComponent> WeaponComponent;

	/** Without this the reactive parry task fails on entry every time. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGParryComponent> ParryComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGPoiseComponent> PoiseComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGInventoryComponent> InventoryComponent;

	/** The other half of "flees and heals". */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGQuickSlotComponent> QuickSlotComponent;

	/**
	 * How loud this NPC is, so OTHER NPCs can hear it.
	 *
	 * Easy to mistake for redundant -- the player is the thing being heard most
	 * of the time -- but hearing is a channel between any two characters, and an
	 * NPC with no noise component is silent to its own allies.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGNoiseComponent> NoiseComponent;

private:
	UPROPERTY()
	TObjectPtr<UARPGVitalSet> VitalSet;

	UPROPERTY()
	TObjectPtr<UARPGOffenseSet> OffenseSet;

	UPROPERTY()
	TObjectPtr<UARPGResistanceSet> ResistanceSet;
};
