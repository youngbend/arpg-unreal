// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGAIController.h"
#include "ARPGBlackboardKeys.h"
#include "ARPGNPCComponent.h"
#include "ARPGNPCDefinition.h"
#include "ARPGNoiseComponent.h"
#include "ARPGPerceptionComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"

namespace ARPGBlackboard
{
	const FName TargetActor(TEXT("TargetActor"));
	const FName LastKnownLocation(TEXT("LastKnownLocation"));
	const FName HomeLocation(TEXT("HomeLocation"));
	const FName IsAlerted(TEXT("IsAlerted"));
	const FName InvestigateLocation(TEXT("InvestigateLocation"));
}

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
	if (Owner->ActorHasTag(TEXT("Attacking")))
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

	// The blackboard component is created here rather than lazily by RunBehaviorTree
	// so that perception -- which writes to it from its first scan -- always has
	// somewhere to write, even before a tree is running.
	Blackboard = CreateDefaultSubobject<UBlackboardComponent>(TEXT("BlackboardComponent"));
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
		}
	}

	if (Definition->BehaviorTree)
	{
		RunBehaviorTree(Definition->BehaviorTree);

		// Where it started, so a leash check has something to measure against.
		// Captured on possession rather than authored, because "home" for a
		// placed enemy is simply where the designer put it.
		if (UBlackboardComponent* BB = GetBlackboardComponent())
		{
			BB->SetValueAsVector(ARPGBlackboard::HomeLocation, InPawn->GetActorLocation());
		}
	}
}

void AARPGAIController::OnUnPossess()
{
	// Senses belong to the mind, not the body -- an unpossessed pawn should not
	// keep being perceived FOR. Clearing here stops a freed pawn's last target
	// leaking into the next thing this controller possesses.
	if (Perception)
	{
		Perception->SetTarget(nullptr);
	}

	Super::OnUnPossess();
}
