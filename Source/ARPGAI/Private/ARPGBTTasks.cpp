// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGBTTasks.h"
#include "ARPGAI.h"
#include "ARPGBlackboardKeys.h"
#include "ARPGComboComponent.h"
#include "ARPGQuickSlotComponent.h"
#include "AIController.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"

namespace
{
	APawn* GetControlledPawn(const UBehaviorTreeComponent& OwnerComp)
	{
		const AAIController* Controller = Cast<AAIController>(OwnerComp.GetAIOwner());
		return Controller ? Controller->GetPawn() : nullptr;
	}

	AActor* GetTarget(const UBehaviorTreeComponent& OwnerComp, const FBlackboardKeySelector& Key)
	{
		const UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
		return Blackboard ? Cast<AActor>(Blackboard->GetValueAsObject(Key.SelectedKeyName)) : nullptr;
	}
}

// ---------------------------------------------------------------------------
// Attack
// ---------------------------------------------------------------------------

UARPGBTTask_Attack::UARPGBTTask_Attack()
{
	NodeName = TEXT("Attack");
}

EBTNodeResult::Type UARPGBTTask_Attack::ExecuteTask(UBehaviorTreeComponent& OwnerComp,
	uint8* NodeMemory)
{
	APawn* Pawn = GetControlledPawn(OwnerComp);
	UARPGComboComponent* Combo =
		Pawn ? Pawn->FindComponentByClass<UARPGComboComponent>() : nullptr;

	if (!Combo)
	{
		return EBTNodeResult::Failed;
	}

	Combo->ReceiveInput(Input);

	// Succeeds on the PRESS, not on the swing landing. The combo component owns
	// the beat, and blocking here would stop the tree re-evaluating its
	// higher-priority branches -- which is precisely when a reactive parry or a
	// flee needs to interrupt.
	return Combo->IsAttacking() ? EBTNodeResult::Succeeded : EBTNodeResult::Failed;
}

FString UARPGBTTask_Attack::GetStaticDescription() const
{
	return FString::Printf(TEXT("Attack (%s)"),
		*StaticEnum<EARPGAttackInput>()->GetNameStringByValue(static_cast<int64>(Input)));
}

// ---------------------------------------------------------------------------
// Face target
// ---------------------------------------------------------------------------

UARPGBTTask_FaceTarget::UARPGBTTask_FaceTarget()
{
	NodeName = TEXT("Face Target");
	bNotifyTick = true;

	TargetKey.AddObjectFilter(this, GET_MEMBER_NAME_CHECKED(
		UARPGBTTask_FaceTarget, TargetKey), AActor::StaticClass());
	TargetKey.SelectedKeyName = ARPGBlackboard::TargetActor;
}

void UARPGBTTask_FaceTarget::InitializeFromAsset(UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);

	if (UBlackboardData* BlackboardAsset = GetBlackboardAsset())
	{
		TargetKey.ResolveSelectedKey(*BlackboardAsset);
	}
}

EBTNodeResult::Type UARPGBTTask_FaceTarget::ExecuteTask(UBehaviorTreeComponent& OwnerComp,
	uint8* NodeMemory)
{
	return GetTarget(OwnerComp, TargetKey) ? EBTNodeResult::InProgress : EBTNodeResult::Failed;
}

void UARPGBTTask_FaceTarget::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory,
	float DeltaSeconds)
{
	APawn* Pawn = GetControlledPawn(OwnerComp);
	const AActor* Target = GetTarget(OwnerComp, TargetKey);

	if (!Pawn || !Target)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	const FVector ToTarget =
		(Target->GetActorLocation() - Pawn->GetActorLocation()).GetSafeNormal2D();
	if (ToTarget.IsNearlyZero())
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
		return;
	}

	const FRotator Current = Pawn->GetActorRotation();
	const FRotator Desired = ToTarget.Rotation();

	const float Remaining = FMath::Abs(FRotator::NormalizeAxis(Desired.Yaw - Current.Yaw));
	if (Remaining <= ToleranceDegrees)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
		return;
	}

	// Interpolated rather than snapped: the turn IS the tell, and an NPC that
	// pivots instantly removes the window a player reads to decide whether to
	// close or back off.
	const FRotator Stepped = FMath::RInterpConstantTo(Current,
		FRotator(Current.Pitch, Desired.Yaw, Current.Roll), DeltaSeconds, TurnSpeedDegrees);

	Pawn->SetActorRotation(Stepped);
}

FString UARPGBTTask_FaceTarget::GetStaticDescription() const
{
	return FString::Printf(TEXT("Face target (within %.0f deg at %.0f deg/s)"),
		ToleranceDegrees, TurnSpeedDegrees);
}

// ---------------------------------------------------------------------------
// Create space
// ---------------------------------------------------------------------------

UARPGBTTask_CreateSpace::UARPGBTTask_CreateSpace()
{
	NodeName = TEXT("Create Space");
	bNotifyTick = true;

	TargetKey.AddObjectFilter(this, GET_MEMBER_NAME_CHECKED(
		UARPGBTTask_CreateSpace, TargetKey), AActor::StaticClass());
	TargetKey.SelectedKeyName = ARPGBlackboard::TargetActor;
}

void UARPGBTTask_CreateSpace::InitializeFromAsset(UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);

	if (UBlackboardData* BlackboardAsset = GetBlackboardAsset())
	{
		TargetKey.ResolveSelectedKey(*BlackboardAsset);
	}
}

EBTNodeResult::Type UARPGBTTask_CreateSpace::ExecuteTask(UBehaviorTreeComponent& OwnerComp,
	uint8* NodeMemory)
{
	FMemory* Memory = reinterpret_cast<FMemory*>(NodeMemory);
	Memory->Elapsed = 0.f;

	return GetTarget(OwnerComp, TargetKey) ? EBTNodeResult::InProgress : EBTNodeResult::Failed;
}

void UARPGBTTask_CreateSpace::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory,
	float DeltaSeconds)
{
	FMemory* Memory = reinterpret_cast<FMemory*>(NodeMemory);
	Memory->Elapsed += DeltaSeconds;

	APawn* Pawn = GetControlledPawn(OwnerComp);
	const AActor* Target = GetTarget(OwnerComp, TargetKey);

	if (!Pawn || !Target)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	const FVector Away = Pawn->GetActorLocation() - Target->GetActorLocation();
	if (Away.Size2D() >= DesiredDistance)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
		return;
	}

	// Gives up rather than backing into terrain forever. Succeeding on timeout
	// rather than failing, because the NPC did retreat -- it just could not
	// retreat as far as it wanted, and failing would send the tree looking for
	// an alternative that does not exist.
	if (Memory->Elapsed >= Timeout)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
		return;
	}

	// A direction, not a destination: a fixed retreat point walks the NPC into a
	// corner and holds it there while the player closes in.
	Pawn->AddMovementInput(Away.GetSafeNormal2D(), 1.f);
}

FString UARPGBTTask_CreateSpace::GetStaticDescription() const
{
	return FString::Printf(TEXT("Back away to %.0f (timeout %.1fs)"), DesiredDistance, Timeout);
}

// ---------------------------------------------------------------------------
// Consumable
// ---------------------------------------------------------------------------

UARPGBTTask_UseConsumable::UARPGBTTask_UseConsumable()
{
	NodeName = TEXT("Use Consumable");
}

EBTNodeResult::Type UARPGBTTask_UseConsumable::ExecuteTask(UBehaviorTreeComponent& OwnerComp,
	uint8* NodeMemory)
{
	APawn* Pawn = GetControlledPawn(OwnerComp);
	UARPGQuickSlotComponent* QuickSlots =
		Pawn ? Pawn->FindComponentByClass<UARPGQuickSlotComponent>() : nullptr;

	if (!QuickSlots)
	{
		return EBTNodeResult::Failed;
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
	return QuickSlots->UseSelected() ? EBTNodeResult::Succeeded : EBTNodeResult::Failed;
}

FString UARPGBTTask_UseConsumable::GetStaticDescription() const
{
	return SlotIndex >= 0
		? FString::Printf(TEXT("Use quick slot %d"), SlotIndex)
		: TEXT("Use selected quick slot");
}
