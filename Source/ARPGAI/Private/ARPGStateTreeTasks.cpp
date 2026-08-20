// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGStateTreeTasks.h"
#include "ARPGAI.h"
#include "ARPGComboComponent.h"
#include "ARPGQuickSlotComponent.h"
#include "ARPGStateTreeContext.h"
#include "AIController.h"
#include "GameFramework/Pawn.h"
#include "NavigationSystem.h"
#include "Navigation/PathFollowingComponent.h"

// ---------------------------------------------------------------------------
// Attack
// ---------------------------------------------------------------------------

EStateTreeRunStatus FARPGStateTreeTask_Attack::EnterState(FStateTreeExecutionContext& Context,
	const FStateTreeTransitionResult& Transition) const
{
	APawn* Pawn = ARPGStateTree::GetPawn(Context);
	UARPGComboComponent* Combo =
		Pawn ? Pawn->FindComponentByClass<UARPGComboComponent>() : nullptr;

	if (!Combo)
	{
		return EStateTreeRunStatus::Failed;
	}

	Combo->ReceiveInput(Input);

	// Succeeds on the PRESS, not on the swing landing -- see the class comment.
	return Combo->IsAttacking() ? EStateTreeRunStatus::Succeeded : EStateTreeRunStatus::Failed;
}

// ---------------------------------------------------------------------------
// Face target
// ---------------------------------------------------------------------------

FARPGStateTreeTask_FaceTarget::FARPGStateTreeTask_FaceTarget()
{
	bShouldCallTick = true;
}

EStateTreeRunStatus FARPGStateTreeTask_FaceTarget::EnterState(FStateTreeExecutionContext& Context,
	const FStateTreeTransitionResult& Transition) const
{
	const FInstanceDataType& Data = Context.GetInstanceData(*this);

	AAIController* Controller = ARPGStateTree::GetController(Context);
	if (!Controller || !Data.Target)
	{
		return EStateTreeRunStatus::Failed;
	}

	// The engine turns the pawn from here on: the controller aims its control
	// rotation at the focus and the movement component walks the actor's yaw
	// toward it at RotationRate. See AARPGAIController::OnPossess for the two
	// movement flags that make that happen.
	Controller->SetFocus(Data.Target);

	return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FARPGStateTreeTask_FaceTarget::Tick(FStateTreeExecutionContext& Context,
	const float DeltaTime) const
{
	const FInstanceDataType& Data = Context.GetInstanceData(*this);

	const APawn* Pawn = ARPGStateTree::GetPawn(Context);
	if (!Pawn || !Data.Target)
	{
		return EStateTreeRunStatus::Failed;
	}

	const FVector ToTarget =
		(Data.Target->GetActorLocation() - Pawn->GetActorLocation()).GetSafeNormal2D();
	if (ToTarget.IsNearlyZero())
	{
		// Standing inside each other: any yaw is as correct as any other, and
		// holding the state open waiting for a turn that cannot be measured would
		// stall the tree.
		return EStateTreeRunStatus::Succeeded;
	}

	const float Remaining = FMath::Abs(FRotator::NormalizeAxis(
		ToTarget.Rotation().Yaw - Pawn->GetActorRotation().Yaw));

	// Only reports; the turn itself is the movement component's job now.
	return Remaining <= ToleranceDegrees
		? EStateTreeRunStatus::Succeeded
		: EStateTreeRunStatus::Running;
}

void FARPGStateTreeTask_FaceTarget::ExitState(FStateTreeExecutionContext& Context,
	const FStateTreeTransitionResult& Transition) const
{
	// Released on the way out, so a later state that wants the pawn to look where
	// it is going is not fighting a focus nobody cleared.
	if (AAIController* Controller = ARPGStateTree::GetController(Context))
	{
		Controller->ClearFocus(EAIFocusPriority::Gameplay);
	}
}

// ---------------------------------------------------------------------------
// Create space
// ---------------------------------------------------------------------------

namespace
{
	/**
	 * The reachable point furthest from the target, within DesiredDistance.
	 *
	 * A SAMPLE RATHER THAN A SOLVE, because the ideal retreat point is whatever
	 * the navmesh actually offers and there is no closed form for that. Sampling
	 * is also what makes the corner case work: with no reachable point directly
	 * away, the best of a handful of random ones is the sideways escape.
	 *
	 * Returns false when the navmesh yields nothing at all -- an NPC standing off
	 * the mesh entirely -- which the caller reports rather than papering over.
	 */
	bool FindRetreatPoint(const APawn& Pawn, const AActor& Target, float DesiredDistance,
		int32 SampleCount, FVector& OutPoint)
	{
		UNavigationSystemV1* Navigation =
			FNavigationSystem::GetCurrent<UNavigationSystemV1>(Pawn.GetWorld());
		if (!Navigation)
		{
			return false;
		}

		const FVector Origin = Pawn.GetActorLocation();
		const FVector TargetLocation = Target.GetActorLocation();

		float BestDistanceSq = -1.f;
		bool bFound = false;

		for (int32 Index = 0; Index < SampleCount; ++Index)
		{
			FNavLocation Sample;
			if (!Navigation->GetRandomReachablePointInRadius(Origin, DesiredDistance, Sample))
			{
				continue;
			}

			const float DistanceSq = FVector::DistSquared2D(Sample.Location, TargetLocation);
			if (DistanceSq > BestDistanceSq)
			{
				BestDistanceSq = DistanceSq;
				OutPoint = Sample.Location;
				bFound = true;
			}
		}

		return bFound;
	}

	/** Issues one retreat leg. False when there was nowhere to go. */
	bool RequestRetreat(AAIController& Controller, const APawn& Pawn, const AActor& Target,
		float DesiredDistance, int32 SampleCount, float AcceptanceRadius)
	{
		FVector Destination = FVector::ZeroVector;
		if (!FindRetreatPoint(Pawn, Target, DesiredDistance, SampleCount, Destination))
		{
			return false;
		}

		const EPathFollowingRequestResult::Type Result = Controller.MoveToLocation(
			Destination, AcceptanceRadius, /*bStopOnOverlap=*/true, /*bUsePathfinding=*/true);

		return Result != EPathFollowingRequestResult::Failed;
	}
}

FARPGStateTreeTask_CreateSpace::FARPGStateTreeTask_CreateSpace()
{
	bShouldCallTick = true;
}

EStateTreeRunStatus FARPGStateTreeTask_CreateSpace::EnterState(FStateTreeExecutionContext& Context,
	const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);
	Data.Elapsed = 0.f;
	Data.SinceDecision = 0.f;
	Data.bRequestedMove = false;

	AAIController* Controller = ARPGStateTree::GetController(Context);
	const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;

	if (!Controller || !Pawn || !Data.Target)
	{
		return EStateTreeRunStatus::Failed;
	}

	// NOTE. RetreatQuery is intentionally not executed yet. Running an EQS asset
	// from here is a handful of lines against UEnvQueryManager, but the query
	// asset itself has to be authored in the editor -- so wiring the call without
	// an asset to run means untested code on a path nothing can reach. The
	// sampler below is navmesh-correct on its own; swap it for the query when
	// there is a query to swap in.
	Data.bRequestedMove = RequestRetreat(*Controller, *Pawn, *Data.Target,
		DesiredDistance, SampleCount, AcceptanceRadius);

	return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FARPGStateTreeTask_CreateSpace::Tick(FStateTreeExecutionContext& Context,
	const float DeltaTime) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);
	Data.Elapsed += DeltaTime;
	Data.SinceDecision += DeltaTime;

	AAIController* Controller = ARPGStateTree::GetController(Context);
	const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;

	if (!Controller || !Pawn || !Data.Target)
	{
		return EStateTreeRunStatus::Failed;
	}

	// The goal is a DISTANCE, so it is checked against the target's live position
	// rather than against arrival: a player who backs off while the NPC retreats
	// has already granted the space the NPC wanted.
	const float Distance =
		FVector::Dist2D(Pawn->GetActorLocation(), Data.Target->GetActorLocation());
	if (Distance >= DesiredDistance)
	{
		return EStateTreeRunStatus::Succeeded;
	}

	// Succeeding on timeout rather than failing, because the NPC did retreat --
	// it just could not retreat as far as it wanted, and failing would send the
	// tree looking for an alternative that does not exist.
	if (Data.Elapsed >= Timeout)
	{
		return EStateTreeRunStatus::Succeeded;
	}

	// A finished or failed leg re-decides against where the target is NOW. This is
	// what keeps the retreat honest as the player chases: one committed
	// destination would have the NPC run to a spot the player is already standing.
	//
	// Rate-limited, because "idle" also covers MoveToLocation reporting
	// AlreadyAtGoal -- so a blocked retreat would otherwise re-sample the navmesh
	// every single frame.
	const bool bLegFinished =
		!Data.bRequestedMove || Controller->GetMoveStatus() == EPathFollowingStatus::Idle;

	if (bLegFinished && Data.SinceDecision >= RedecideInterval)
	{
		Data.SinceDecision = 0.f;
		Data.bRequestedMove = RequestRetreat(*Controller, *Pawn, *Data.Target,
			DesiredDistance, SampleCount, AcceptanceRadius);

		if (!Data.bRequestedMove)
		{
			// Nowhere on the navmesh to go. Reported as success for the same reason
			// as the timeout: the tree cannot fix this by trying something else.
			UE_LOG(LogARPGAI, Verbose,
				TEXT("%s wanted to back away but the navmesh offered nowhere to go."),
				*GetNameSafe(Pawn));
			return EStateTreeRunStatus::Succeeded;
		}
	}

	return EStateTreeRunStatus::Running;
}

void FARPGStateTreeTask_CreateSpace::ExitState(FStateTreeExecutionContext& Context,
	const FStateTreeTransitionResult& Transition) const
{
	const FInstanceDataType& Data = Context.GetInstanceData(*this);

	// Stopped explicitly: leaving a path following request live would have the
	// pawn keep walking backwards through whatever state comes next.
	if (Data.bRequestedMove)
	{
		if (AAIController* Controller = ARPGStateTree::GetController(Context))
		{
			Controller->StopMovement();
		}
	}
}

// ---------------------------------------------------------------------------
// Consumable
// ---------------------------------------------------------------------------

EStateTreeRunStatus FARPGStateTreeTask_UseConsumable::EnterState(
	FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	APawn* Pawn = ARPGStateTree::GetPawn(Context);
	UARPGQuickSlotComponent* QuickSlots =
		Pawn ? Pawn->FindComponentByClass<UARPGQuickSlotComponent>() : nullptr;

	if (!QuickSlots)
	{
		return EStateTreeRunStatus::Failed;
	}

	// An NPC has no thumbs: it addresses a slot directly rather than cycling to
	// it the way the player does.
	if (SlotIndex >= 0)
	{
		QuickSlots->SetSelectedIndex(SlotIndex);
	}

	// Fails when out of stock or still on cooldown, which is what lets a tree
	// fall through to a different response rather than standing there drinking
	// from an empty flask.
	return QuickSlots->UseSelected()
		? EStateTreeRunStatus::Succeeded
		: EStateTreeRunStatus::Failed;
}
