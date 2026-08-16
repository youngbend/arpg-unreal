// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGAIController.h"
#include "ARPGGameplayTags.h"
#include "ARPGNPCComponent.h"
#include "ARPGNPCDefinition.h"
#include "ARPGNoiseComponent.h"
#include "ARPGPerceptionComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Components/StateTreeAIComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"

// ---------------------------------------------------------------------------
// Noise
// ---------------------------------------------------------------------------

UARPGNoiseComponent::UARPGNoiseComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

float UARPGNoiseComponent::GetCurrentNoiseRadius() const
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return 0.f;
	}

	const float Speed = Owner->GetVelocity().Size2D();

	float Radius = IdleRadius;
	if (Speed >= SprintSpeedThreshold)
	{
		Radius = SprintRadius;
	}
	else if (Speed >= IdleSpeedThreshold)
	{
		Radius = WalkRadius;
	}

	// A FLOOR, not a replacement: someone sprinting and swinging is at least as
	// loud as sprinting. Taking the max is what keeps the two independent.
	//
	// THE GAMEPLAY TAG, not an actor tag. This read ActorHasTag("Attacking"),
	// which nothing in the project has ever set -- so the attack floor was dead
	// code and a swinging character was exactly as loud as a standing one. The
	// melee ability owns State.Attacking for as long as it is active, which is
	// the fact the port meant to consult all along.
	const UAbilitySystemComponent* ASC =
		UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Owner);

	if (ASC && ASC->HasMatchingGameplayTag(TAG_State_Attacking))
	{
		Radius = FMath::Max(Radius, AttackRadius);
	}

	return Radius;
}

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------

AARPGAIController::AARPGAIController()
{
	Perception = CreateDefaultSubobject<UARPGPerceptionComponent>(TEXT("Perception"));

	// No blackboard is created here. Under behaviour trees one was, eagerly, so
	// that perception had somewhere to write from its first scan; StateTree binds
	// properties instead and the evaluator reads perception directly.
	StateTreeAI = CreateDefaultSubobject<UStateTreeAIComponent>(TEXT("StateTreeAI"));
}

UARPGNPCDefinition* AARPGAIController::GetNPCDefinition() const
{
	const APawn* ControlledPawn = GetPawn();
	if (!ControlledPawn)
	{
		return nullptr;
	}

	// Read off the pawn rather than held here, so possessing a different pawn
	// picks up its archetype rather than keeping the previous one's.
	const UARPGNPCComponent* NPC = ControlledPawn->FindComponentByClass<UARPGNPCComponent>();
	return NPC ? NPC->Definition : nullptr;
}

void AARPGAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	UARPGNPCDefinition* Definition = GetNPCDefinition();
	if (!Definition)
	{
		return;
	}

	// Perception is configured from the archetype, so an NPC's senses are part
	// of what it IS rather than something set up per placed instance.
	Perception->DetectionRange = Definition->DetectionRange;
	Perception->DamageReaction = Definition->DamageReaction;

	if (ACharacter* PossessedCharacter = Cast<ACharacter>(InPawn))
	{
		if (UCharacterMovementComponent* Movement = PossessedCharacter->GetCharacterMovement())
		{
			Movement->MaxWalkSpeed = Definition->MoveSpeed;

			// THE TWO FLAGS THE FOCUS SYSTEM NEEDS. With bUseControllerDesiredRotation
			// the movement component walks the pawn's yaw toward the controller's
			// desired rotation at RotationRate, and AAIController aims that rotation
			// at whatever SetFocus was given. Orient-to-movement has to be off or the
			// two fight: one wants the pawn facing its velocity, the other facing its
			// target, and a fleeing NPC needs to be looking at what it is fleeing.
			Movement->bUseControllerDesiredRotation = true;
			Movement->bOrientRotationToMovement = false;
			Movement->RotationRate = FRotator(0.f, Definition->TurnRateDegrees, 0.f);
		}
	}

	// Where it started is captured by FARPGStateTreeEvaluator_Perception on tree
	// start rather than written here, so "home" arrives through the same binding
	// as everything else the tree reads.
	if (StateTreeAI && Definition->StateTreeRef.IsValid())
	{
		// The one call to re-check against the installed engine version if this
		// module fails to compile: assigning a tree at runtime is what makes the
		// archetype data rather than a per-Blueprint default.
		StateTreeAI->SetStateTreeReference(Definition->StateTreeRef);
		StateTreeAI->StartLogic();
	}
}

void AARPGAIController::OnUnPossess()
{
	// Stopped before the pawn goes: a tree left running would keep ticking tasks
	// that reach through the controller for a body that is no longer there.
	if (StateTreeAI)
	{
		StateTreeAI->StopLogic(TEXT("Unpossessed"));
	}

	// Senses belong to the mind, not the body -- an unpossessed pawn should not
	// keep being perceived FOR. Clearing here stops a freed pawn's last target
	// leaking into the next thing this controller possesses.
	if (Perception)
	{
		Perception->SetTarget(nullptr);
	}

	Super::OnUnPossess();
}
