// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGBTDecorators.h"
#include "ARPGBlackboardKeys.h"
#include "ARPGVitalSet.h"
#include "ARPGWeaponComponent.h"
#include "ARPGWeaponDefinition.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "AIController.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "GameFramework/Pawn.h"

namespace
{
	/** The pawn this tree is driving, or null. */
	APawn* GetControlledPawn(const UBehaviorTreeComponent& OwnerComp)
	{
		const AAIController* Controller = Cast<AAIController>(OwnerComp.GetAIOwner());
		return Controller ? Controller->GetPawn() : nullptr;
	}
}

// ---------------------------------------------------------------------------
// Attack range
// ---------------------------------------------------------------------------

UARPGBTDecorator_TargetInAttackRange::UARPGBTDecorator_TargetInAttackRange()
{
	NodeName = TEXT("Target In Attack Range");
	TargetKey.AddObjectFilter(this, GET_MEMBER_NAME_CHECKED(
		UARPGBTDecorator_TargetInAttackRange, TargetKey), AActor::StaticClass());
	TargetKey.SelectedKeyName = ARPGBlackboard::TargetActor;
}

void UARPGBTDecorator_TargetInAttackRange::InitializeFromAsset(UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);

	if (UBlackboardData* BlackboardAsset = GetBlackboardAsset())
	{
		TargetKey.ResolveSelectedKey(*BlackboardAsset);
	}
}

bool UARPGBTDecorator_TargetInAttackRange::CalculateRawConditionValue(
	UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const
{
	const APawn* Pawn = GetControlledPawn(OwnerComp);
	const UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!Pawn || !Blackboard)
	{
		return false;
	}

	const AActor* Target = Cast<AActor>(Blackboard->GetValueAsObject(TargetKey.SelectedKeyName));
	if (!Target)
	{
		return false;
	}

	// Reach comes from the equipped weapon, so one tree serves a dagger NPC and
	// a greatsword one without either needing its own copy.
	float Reach = ReachTolerance;
	if (const UARPGWeaponComponent* Weapon = Pawn->FindComponentByClass<UARPGWeaponComponent>())
	{
		if (const UARPGWeaponDefinition* Definition = Weapon->GetWeapon())
		{
			Reach += Definition->Reach;
		}
	}

	return FVector::Dist(Pawn->GetActorLocation(), Target->GetActorLocation()) <= Reach;
}

FString UARPGBTDecorator_TargetInAttackRange::GetStaticDescription() const
{
	return FString::Printf(TEXT("Target within weapon reach + %.0f"), ReachTolerance);
}

// ---------------------------------------------------------------------------
// Facing
// ---------------------------------------------------------------------------

UARPGBTDecorator_FacingTarget::UARPGBTDecorator_FacingTarget()
{
	NodeName = TEXT("Facing Target");
	TargetKey.AddObjectFilter(this, GET_MEMBER_NAME_CHECKED(
		UARPGBTDecorator_FacingTarget, TargetKey), AActor::StaticClass());
	TargetKey.SelectedKeyName = ARPGBlackboard::TargetActor;
}

void UARPGBTDecorator_FacingTarget::InitializeFromAsset(UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);

	if (UBlackboardData* BlackboardAsset = GetBlackboardAsset())
	{
		TargetKey.ResolveSelectedKey(*BlackboardAsset);
	}
}

bool UARPGBTDecorator_FacingTarget::CalculateRawConditionValue(
	UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const
{
	const APawn* Pawn = GetControlledPawn(OwnerComp);
	const UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!Pawn || !Blackboard)
	{
		return false;
	}

	const AActor* Target = Cast<AActor>(Blackboard->GetValueAsObject(TargetKey.SelectedKeyName));
	if (!Target)
	{
		return false;
	}

	// Horizontal only. A target on a ledge above is still "faced" -- verticality
	// is the aim system's problem, not the decision to swing.
	const FVector Facing = Pawn->GetActorForwardVector().GetSafeNormal2D();
	const FVector ToTarget =
		(Target->GetActorLocation() - Pawn->GetActorLocation()).GetSafeNormal2D();

	if (Facing.IsNearlyZero() || ToTarget.IsNearlyZero())
	{
		return false;
	}

	const float AngleDegrees = FMath::RadiansToDegrees(
		FMath::Acos(FMath::Clamp(FVector::DotProduct(Facing, ToTarget), -1.f, 1.f)));

	return AngleDegrees <= MaxAngleDegrees;
}

FString UARPGBTDecorator_FacingTarget::GetStaticDescription() const
{
	return FString::Printf(TEXT("Facing target within %.0f degrees"), MaxAngleDegrees);
}

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------

UARPGBTDecorator_HealthBelow::UARPGBTDecorator_HealthBelow()
{
	NodeName = TEXT("Health Below");
}

bool UARPGBTDecorator_HealthBelow::CalculateRawConditionValue(
	UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const
{
	const APawn* Pawn = GetControlledPawn(OwnerComp);
	const UAbilitySystemComponent* ASC =
		UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Pawn);
	if (!ASC)
	{
		return false;
	}

	const float MaxHealth = ASC->GetNumericAttribute(UARPGVitalSet::GetMaxHealthAttribute());
	if (MaxHealth <= 0.f)
	{
		// Guarding the divide rather than treating it as "definitely hurt": a
		// character with no max health is misconfigured, and sending it to flee
		// and heal would hide that.
		return false;
	}

	const float Health = ASC->GetNumericAttribute(UARPGVitalSet::GetHealthAttribute());
	return (Health / MaxHealth) < Threshold;
}

FString UARPGBTDecorator_HealthBelow::GetStaticDescription() const
{
	return FString::Printf(TEXT("Health below %.0f%%"), Threshold * 100.f);
}

// ---------------------------------------------------------------------------
// Leash
// ---------------------------------------------------------------------------

UARPGBTDecorator_IsLeashed::UARPGBTDecorator_IsLeashed()
{
	NodeName = TEXT("Is Leashed");
	HomeKey.AddVectorFilter(this, GET_MEMBER_NAME_CHECKED(
		UARPGBTDecorator_IsLeashed, HomeKey));
	HomeKey.SelectedKeyName = ARPGBlackboard::HomeLocation;
}

void UARPGBTDecorator_IsLeashed::InitializeFromAsset(UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);

	if (UBlackboardData* BlackboardAsset = GetBlackboardAsset())
	{
		HomeKey.ResolveSelectedKey(*BlackboardAsset);
	}
}

bool UARPGBTDecorator_IsLeashed::CalculateRawConditionValue(
	UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const
{
	const APawn* Pawn = GetControlledPawn(OwnerComp);
	const UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!Pawn || !Blackboard)
	{
		return false;
	}

	FMemory* Memory = reinterpret_cast<FMemory*>(NodeMemory);

	const FVector Home = Blackboard->GetValueAsVector(HomeKey.SelectedKeyName);
	const float Distance = FVector::Dist(Pawn->GetActorLocation(), Home);

	// Hysteresis: once leashed, stay leashed until back inside the smaller
	// radius. Without it an NPC sitting exactly on the boundary flips between
	// chasing and returning every tick, which reads as a twitch.
	if (Memory->bLeashed)
	{
		const float Release = ReleaseRange > 0.f ? ReleaseRange : LeashRange;
		Memory->bLeashed = Distance > Release;
	}
	else
	{
		Memory->bLeashed = Distance > LeashRange;
	}

	return Memory->bLeashed;
}

FString UARPGBTDecorator_IsLeashed::GetStaticDescription() const
{
	return ReleaseRange > 0.f
		? FString::Printf(TEXT("Further than %.0f from home (releases at %.0f)"),
			LeashRange, ReleaseRange)
		: FString::Printf(TEXT("Further than %.0f from home"), LeashRange);
}
