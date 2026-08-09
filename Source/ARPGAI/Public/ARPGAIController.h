// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "ARPGAIController.generated.h"

class UARPGNPCDefinition;
class UARPGPerceptionComponent;

/**
 * Runs one NPC's behaviour tree and owns its perception.
 *
 * PERCEPTION LIVES ON THE CONTROLLER, not the pawn. That is the UE convention
 * and it is also correct here: perception is knowledge, and knowledge belongs to
 * the mind rather than the body. It also means a possessed pawn keeps its
 * senses' configuration when swapped, and an unpossessed one has none at all --
 * which is what "nobody is driving this" should mean.
 *
 * The tree comes from the pawn's UARPGNPCDefinition, so an archetype is still
 * entirely an asset. See UARPGNPCDefinition.
 */
UCLASS()
class ARPGAI_API AARPGAIController : public AAIController
{
	GENERATED_BODY()

public:
	AARPGAIController();

	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;

	UFUNCTION(BlueprintPure, Category = "ARPG|AI")
	UARPGPerceptionComponent* GetARPGPerception() const { return Perception; }

	/** The definition of whatever this controller is currently driving. */
	UFUNCTION(BlueprintPure, Category = "ARPG|AI")
	UARPGNPCDefinition* GetNPCDefinition() const;

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGPerceptionComponent> Perception;
};
