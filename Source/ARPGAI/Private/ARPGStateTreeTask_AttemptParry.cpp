// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGStateTreeTask_AttemptParry.h"
#include "ARPGAI.h"
#include "ARPGAILibrary.h"
#include "ARPGComboComponent.h"
#include "ARPGCombatTypes.h"
#include "ARPGHitboxComponent.h"
#include "ARPGParryComponent.h"
#include "ARPGStateTreeContext.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"

namespace
{
	/** Below this, the hitbox is drifting rather than swinging. */
	constexpr float MinClosingSpeed = 50.f;

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

		const float PlayRate =
			FMath::Max(KINDA_SMALL_NUMBER, AnimInstance->Montage_GetPlayRate(Montage));
		return (ActiveStart - Position) / PlayRate;
	}
}

FARPGStateTreeTask_AttemptParry::FARPGStateTreeTask_AttemptParry()
{
	bShouldCallTick = true;
}

EStateTreeRunStatus FARPGStateTreeTask_AttemptParry::EnterState(
	FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);

	// Reset everything but the bound Target, which the binding owns.
	Data.TriggerThreshold = 0.f;
	Data.PreviousDistance = -1.f;
	Data.PreviousClosingSpeed = 0.f;
	Data.SmoothedAcceleration = 0.f;
	Data.bTriggered = false;
	Data.HoldRemaining = 0.f;

	APawn* Pawn = ARPGStateTree::GetPawn(Context);
	if (!Pawn || !Data.Target)
	{
		return EStateTreeRunStatus::Failed;
	}

	UARPGParryComponent* Parry = Pawn->FindComponentByClass<UARPGParryComponent>();
	if (!Parry)
	{
		return EStateTreeRunStatus::Failed;
	}

	// Nothing to react to. Failing rather than running lets the tree fall straight
	// through to a normal attack state on the same tick.
	const UARPGComboComponent* TargetCombo =
		Data.Target->FindComponentByClass<UARPGComboComponent>();
	if (!TargetCombo || !TargetCombo->IsAttacking())
	{
		return EStateTreeRunStatus::Failed;
	}

	// Finishing an authored combo's payoff hit takes priority over a reactive
	// parry -- otherwise the NPC abandons its own commitment every time the
	// player so much as starts a swing.
	const UARPGComboComponent* OwnCombo = Pawn->FindComponentByClass<UARPGComboComponent>();
	if (OwnCombo && OwnCombo->IsAttacking())
	{
		return EStateTreeRunStatus::Failed;
	}

	// DERIVED, not authored: centring the perfect window on predicted contact
	// leaves equal slack for prediction error either way. Guard timings live on
	// the parry component and are never duplicated here.
	//
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

	Data.TriggerThreshold = FMath::Max(0.f,
		Parry->BlendInTime + 0.5f * Parry->ParryWindow + ReactionDelay + Jitter);

	return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FARPGStateTreeTask_AttemptParry::Tick(FStateTreeExecutionContext& Context,
	const float DeltaTime) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);

	APawn* Pawn = ARPGStateTree::GetPawn(Context);
	UARPGParryComponent* Parry =
		Pawn ? Pawn->FindComponentByClass<UARPGParryComponent>() : nullptr;

	if (!Pawn || !Parry || !Data.Target)
	{
		return EStateTreeRunStatus::Failed;
	}

	// --- Holding ------------------------------------------------------------
	if (Data.bTriggered)
	{
		Data.HoldRemaining -= DeltaTime;
		if (Data.HoldRemaining <= 0.f)
		{
			Parry->EndBlock();
			return EStateTreeRunStatus::Succeeded;
		}
		return EStateTreeRunStatus::Running;
	}

	// --- Predicting ---------------------------------------------------------
	bool bShouldTrigger = false;

	// bAllowFallback FALSE: this is asking about one specific hitbox, the weapon
	// whose approach it is timing against. Falling back to a body hitbox would
	// have the parry solve for the arrival of a foot.
	const UARPGHitboxComponent* TargetHitbox = UARPGHitboxComponent::FindOnActor(
		Data.Target, EARPGHitboxSource::Weapon, /*bAllowFallback=*/false);
	if (TargetHitbox)
	{
		const float Distance =
			FVector::Dist(Pawn->GetActorLocation(), TargetHitbox->GetComponentLocation());

		if (Data.PreviousDistance >= 0.f && DeltaTime > KINDA_SMALL_NUMBER)
		{
			const float ClosingSpeed = (Data.PreviousDistance - Distance) / DeltaTime;
			const float RawAcceleration = (ClosingSpeed - Data.PreviousClosingSpeed) / DeltaTime;

			// Smoothed, because a real swing's closing speed ramps sharply in the
			// last handful of frames and the raw second difference of a sampled
			// position is extremely noisy.
			Data.SmoothedAcceleration =
				FMath::Lerp(Data.SmoothedAcceleration, RawAcceleration, 0.3f);

			const float TimeToContact = UARPGAILibrary::PredictTimeToContact(
				Distance, ClosingSpeed, Data.SmoothedAcceleration, ContactRadius);

			// Only trust a prediction while the hitbox is GENUINELY closing.
			// During the mid-swing hover -- the weapon drifting between wind-back
			// and the real strike -- closing speed sits near zero while the noisy
			// acceleration estimate stays mildly positive, which yields plausible
			// but wrong predictions purely from the a*t^2/2 term. Firing on those
			// reacts hundreds of milliseconds early, so the whole block has
			// expired by the time the hit actually lands.
			const bool bApproachIsReal = ClosingSpeed >= MinClosingSpeed;

			if (bApproachIsReal && TimeToContact >= 0.f
				&& TimeToContact <= Data.TriggerThreshold * ClosingSpeedMargin)
			{
				bShouldTrigger = true;
			}
		}

		Data.PreviousClosingSpeed = Data.PreviousDistance >= 0.f && DeltaTime > KINDA_SMALL_NUMBER
			? (Data.PreviousDistance - Distance) / DeltaTime
			: 0.f;
		Data.PreviousDistance = Distance;
	}
	else
	{
		// No hitbox to track: fall back to the montage's own section timing.
		const float TimeUntilActive = GetTimeUntilActiveSection(Data.Target);
		if (TimeUntilActive >= 0.f && TimeUntilActive <= Data.TriggerThreshold)
		{
			bShouldTrigger = true;
		}
	}

	if (!bShouldTrigger)
	{
		return EStateTreeRunStatus::Running;
	}

	Parry->BeginBlock();
	Data.bTriggered = true;

	// Held past the window by the buffer, so a slightly late prediction still
	// blocks (worse than a parry, far better than a clean hit).
	Data.HoldRemaining = Parry->BlendInTime + Parry->ParryWindow + HoldBuffer;

	UE_LOG(LogARPGAI, Verbose, TEXT("%s is parrying (threshold %.3fs)"),
		*GetNameSafe(Pawn), Data.TriggerThreshold);

	return EStateTreeRunStatus::Running;
}
