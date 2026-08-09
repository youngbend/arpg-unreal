// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGBTTask_AttemptParry.h"
#include "ARPGAI.h"
#include "ARPGBlackboardKeys.h"
#include "ARPGComboComponent.h"
#include "ARPGCombatTypes.h"
#include "ARPGHitboxComponent.h"
#include "ARPGParryComponent.h"
#include "AIController.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"

namespace
{
	/** Below this, the hitbox is drifting rather than swinging. */
	constexpr float MinClosingSpeed = 50.f;

	APawn* GetControlledPawn(const UBehaviorTreeComponent& OwnerComp)
	{
		const AAIController* Controller = Cast<AAIController>(OwnerComp.GetAIOwner());
		return Controller ? Controller->GetPawn() : nullptr;
	}

	/** The weapon hitbox of whatever this actor is swinging, if any. */
	UARPGHitboxComponent* FindWeaponHitbox(const AActor* Actor)
	{
		if (!Actor)
		{
			return nullptr;
		}

		TArray<UARPGHitboxComponent*> Hitboxes;
		Actor->GetComponents<UARPGHitboxComponent>(Hitboxes);

		for (UARPGHitboxComponent* Hitbox : Hitboxes)
		{
			if (Hitbox->HitboxSource == EARPGHitboxSource::Weapon)
			{
				return Hitbox;
			}
		}
		return nullptr;
	}

	/**
	 * Seconds until the target's current montage reaches its Active section.
	 *
	 * THE PHASE 4 PAYOFF. Godot's AttackDefinition stored clip NAMES with no
	 * access to durations, so animation.gd had to publish phase timings every
	 * frame purely so this node could read them. A montage knows its own section
	 * times, so the whole channel disappears into one query.
	 *
	 * Negative when the target is not mid-attack or the montage has no Active
	 * section to aim at.
	 */
	float GetTimeUntilActiveSection(const AActor* Target)
	{
		const ACharacter* Character = Cast<ACharacter>(Target);
		const USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
		const UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;

		if (!AnimInstance)
		{
			return -1.f;
		}

		UAnimMontage* Montage = AnimInstance->GetCurrentActiveMontage();
		if (!Montage)
		{
			return -1.f;
		}

		const int32 ActiveIndex = Montage->GetSectionIndex(ARPGMontageSections::Active);
		if (ActiveIndex == INDEX_NONE)
		{
			return -1.f;
		}

		float ActiveStart = 0.f;
		float ActiveEnd = 0.f;
		Montage->GetSectionStartAndEndTime(ActiveIndex, ActiveStart, ActiveEnd);

		const float Position = AnimInstance->Montage_GetPosition(Montage);
		if (Position < 0.f)
		{
			return -1.f;
		}

		// Already in or past the active window: contact is imminent, not future.
		if (Position >= ActiveStart)
		{
			return 0.f;
		}

		const float PlayRate = FMath::Max(KINDA_SMALL_NUMBER, AnimInstance->Montage_GetPlayRate(Montage));
		return (ActiveStart - Position) / PlayRate;
	}
}

UARPGBTTask_AttemptParry::UARPGBTTask_AttemptParry()
{
	NodeName = TEXT("Attempt Parry");
	bNotifyTick = true;

	TargetKey.AddObjectFilter(this, GET_MEMBER_NAME_CHECKED(
		UARPGBTTask_AttemptParry, TargetKey), AActor::StaticClass());
	TargetKey.SelectedKeyName = ARPGBlackboard::TargetActor;
}

void UARPGBTTask_AttemptParry::InitializeFromAsset(UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);

	if (UBlackboardData* BlackboardAsset = GetBlackboardAsset())
	{
		TargetKey.ResolveSelectedKey(*BlackboardAsset);
	}
}

float UARPGBTTask_AttemptParry::PredictTimeToContact(float Distance, float ClosingSpeed,
	float Acceleration, float InContactRadius)
{
	const float RemainingDistance = FMath::Max(0.f, Distance - InContactRadius);

	if (RemainingDistance <= 0.f)
	{
		return 0.f; // already touching
	}

	// Effectively constant speed: a plain linear solve, and no acceleration term
	// to amplify sampling noise.
	if (FMath::Abs(Acceleration) < 1.f)
	{
		return ClosingSpeed > KINDA_SMALL_NUMBER ? RemainingDistance / ClosingSpeed : -1.f;
	}

	// d = v*t + a*t^2/2, solved for t.
	const float Discriminant = ClosingSpeed * ClosingSpeed + 2.f * Acceleration * RemainingDistance;
	if (Discriminant < 0.f)
	{
		// At the current (decelerating) rate this approach never reaches contact
		// at all. Correctly no prediction rather than a guess.
		return -1.f;
	}

	const float Root = FMath::Sqrt(Discriminant);
	const float First = (-ClosingSpeed + Root) / Acceleration;
	const float Second = (-ClosingSpeed - Root) / Acceleration;

	// The smallest non-negative root: the FIRST time contact is reached.
	float Best = -1.f;
	if (First >= 0.f)
	{
		Best = First;
	}
	if (Second >= 0.f && (Best < 0.f || Second < Best))
	{
		Best = Second;
	}

	return Best;
}

EBTNodeResult::Type UARPGBTTask_AttemptParry::ExecuteTask(UBehaviorTreeComponent& OwnerComp,
	uint8* NodeMemory)
{
	FMemory* Memory = reinterpret_cast<FMemory*>(NodeMemory);
	*Memory = FMemory();

	APawn* Pawn = GetControlledPawn(OwnerComp);
	const UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!Pawn || !Blackboard)
	{
		return EBTNodeResult::Failed;
	}

	const AActor* Target = Cast<AActor>(Blackboard->GetValueAsObject(TargetKey.SelectedKeyName));
	UARPGParryComponent* Parry = Pawn->FindComponentByClass<UARPGParryComponent>();

	if (!Target || !Parry)
	{
		return EBTNodeResult::Failed;
	}

	// Nothing to react to. Failing rather than running lets a selector fall
	// straight through to a normal attack branch on the same tick.
	const UARPGComboComponent* TargetCombo = Target->FindComponentByClass<UARPGComboComponent>();
	if (!TargetCombo || !TargetCombo->IsAttacking())
	{
		return EBTNodeResult::Failed;
	}

	// Finishing an authored combo's payoff hit takes priority over a reactive
	// parry -- otherwise the NPC abandons its own commitment every time the
	// player so much as starts a swing.
	const UARPGComboComponent* OwnCombo = Pawn->FindComponentByClass<UARPGComboComponent>();
	if (OwnCombo && OwnCombo->IsAttacking())
	{
		return EBTNodeResult::Failed;
	}

	// DERIVED, not authored: centring the perfect window on predicted contact
	// leaves equal slack for prediction error either way. Guard timings live on
	// the parry component and are never duplicated here.
	// Box-Muller rather than a uniform spread: reaction time is a bell, and a
	// uniform one gives too many extreme reactions at both ends -- an NPC that
	// is either superhuman or asleep, rather than usually about right.
	float Jitter = 0.f;
	if (ReactionJitter > 0.f)
	{
		const float U1 = FMath::Max(KINDA_SMALL_NUMBER, FMath::FRand());
		const float U2 = FMath::FRand();
		Jitter = ReactionJitter * FMath::Sqrt(-2.f * FMath::Loge(U1))
			* FMath::Cos(2.f * PI * U2);
	}

	Memory->TriggerThreshold = FMath::Max(0.f,
		Parry->BlendInTime + 0.5f * Parry->ParryWindow + ReactionDelay + Jitter);

	return EBTNodeResult::InProgress;
}

void UARPGBTTask_AttemptParry::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory,
	float DeltaSeconds)
{
	FMemory* Memory = reinterpret_cast<FMemory*>(NodeMemory);

	APawn* Pawn = GetControlledPawn(OwnerComp);
	const UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	UARPGParryComponent* Parry = Pawn ? Pawn->FindComponentByClass<UARPGParryComponent>() : nullptr;

	if (!Pawn || !Blackboard || !Parry)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	const AActor* Target = Cast<AActor>(Blackboard->GetValueAsObject(TargetKey.SelectedKeyName));
	if (!Target)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	// --- Holding ------------------------------------------------------------
	if (Memory->bTriggered)
	{
		Memory->HoldRemaining -= DeltaSeconds;
		if (Memory->HoldRemaining <= 0.f)
		{
			Parry->EndBlock();
			FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
		}
		return;
	}

	// --- Predicting ---------------------------------------------------------
	bool bShouldTrigger = false;

	const UARPGHitboxComponent* TargetHitbox = FindWeaponHitbox(Target);
	if (TargetHitbox)
	{
		const float Distance =
			FVector::Dist(Pawn->GetActorLocation(), TargetHitbox->GetComponentLocation());

		if (Memory->PreviousDistance >= 0.f && DeltaSeconds > KINDA_SMALL_NUMBER)
		{
			const float ClosingSpeed = (Memory->PreviousDistance - Distance) / DeltaSeconds;
			const float RawAcceleration = (ClosingSpeed - Memory->PreviousClosingSpeed) / DeltaSeconds;

			// Smoothed, because a real swing's closing speed ramps sharply in the
			// last handful of frames and the raw second difference of a sampled
			// position is extremely noisy.
			Memory->SmoothedAcceleration =
				FMath::Lerp(Memory->SmoothedAcceleration, RawAcceleration, 0.3f);

			const float TimeToContact = PredictTimeToContact(Distance, ClosingSpeed,
				Memory->SmoothedAcceleration, ContactRadius);

			// Only trust a prediction while the hitbox is GENUINELY closing.
			// During the mid-swing hover -- the weapon drifting between wind-back
			// and the real strike -- closing speed sits near zero while the noisy
			// acceleration estimate stays mildly positive, which yields plausible
			// but wrong predictions purely from the a*t^2/2 term. Firing on those
			// reacts hundreds of milliseconds early, so the whole block has
			// expired by the time the hit actually lands.
			const bool bApproachIsReal = ClosingSpeed >= MinClosingSpeed;

			if (bApproachIsReal && TimeToContact >= 0.f
				&& TimeToContact <= Memory->TriggerThreshold * ClosingSpeedMargin)
			{
				bShouldTrigger = true;
			}
		}

		Memory->PreviousClosingSpeed = Memory->PreviousDistance >= 0.f && DeltaSeconds > KINDA_SMALL_NUMBER
			? (Memory->PreviousDistance - Distance) / DeltaSeconds
			: 0.f;
		Memory->PreviousDistance = Distance;
	}
	else
	{
		// No hitbox to track: fall back to the montage's own section timing.
		const float TimeUntilActive = GetTimeUntilActiveSection(Target);
		if (TimeUntilActive >= 0.f && TimeUntilActive <= Memory->TriggerThreshold)
		{
			bShouldTrigger = true;
		}
	}

	if (!bShouldTrigger)
	{
		return;
	}

	Parry->BeginBlock();
	Memory->bTriggered = true;

	// Held past the window by the buffer, so a slightly late prediction still
	// blocks (worse than a parry, far better than a clean hit).
	Memory->HoldRemaining = Parry->BlendInTime + Parry->ParryWindow + HoldBuffer;

	UE_LOG(LogARPGAI, Verbose, TEXT("%s is parrying (threshold %.3fs)"),
		*GetNameSafe(Pawn), Memory->TriggerThreshold);
}

FString UARPGBTTask_AttemptParry::GetStaticDescription() const
{
	return FString::Printf(TEXT("Attempt parry (lead %.2fs +/- %.2f)"),
		ReactionDelay, ReactionJitter);
}
