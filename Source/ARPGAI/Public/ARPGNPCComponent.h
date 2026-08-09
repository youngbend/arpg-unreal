// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ARPGNPCComponent.generated.h"

class UARPGNPCDefinition;

/**
 * Makes a pawn an NPC of a particular archetype. Port of the glue in Godot's
 * NPCCharacter.
 *
 * A COMPONENT RATHER THAN A CHARACTER SUBCLASS, so an NPC is a pawn that has one
 * of these rather than a pawn that inherits from something. That matters because
 * the archetype is data: the same character class serves every enemy in the
 * game, and adding a goblin never touches C++ or a Blueprint hierarchy.
 *
 * It does two jobs the controller cannot: it carries the definition (the
 * controller reads it on possession), and it forwards hits to perception, which
 * is how being struck by someone unseen turns into either aggro or an
 * investigation.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGAI_API UARPGNPCComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGNPCComponent();

	virtual void BeginPlay() override;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|NPC")
	TObjectPtr<UARPGNPCDefinition> Definition;

protected:
	/**
	 * Applies the archetype's combat numbers and weapon.
	 *
	 * Server-only: these write attributes, and a client doing so would fight its
	 * own replication.
	 */
	void ApplyDefinition();

	UFUNCTION()
	void HandleHitReceived(const FGameplayEffectContextHandle& Context, float Magnitude);
};
