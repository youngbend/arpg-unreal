// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGPerceptionComponent.h"
#include "ARPGAI.h"
#include "ARPGBlackboardKeys.h"
#include "ARPGCombatLibrary.h"
#include "ARPGGameplayTags.h"
#include "ARPGNoiseComponent.h"
#include "ARPGThreatRegistry.h"
#include "AIController.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"

UARPGPerceptionComponent::UARPGPerceptionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// The engine's own interval, rather than the hand-rolled accumulator this
	// used to carry: it skips the dispatch entirely instead of entering the
	// function to discover there is nothing to do. Kept in step with ScanInterval
	// by BeginPlay, so the authored value still wins.
	PrimaryComponentTick.TickInterval = ScanInterval;
}

void UARPGPerceptionComponent::BeginPlay()
{
	Super::BeginPlay();

	// ScanInterval is EditAnywhere, so the constructor's value is only a default.
	SetComponentTickInterval(FMath::Max(0.f, ScanInterval));
}

void UARPGPerceptionComponent::PublishTargetChange(AActor* OldTarget, AActor* NewTarget) const
{
	if (const UWorld* World = GetWorld())
	{
		if (UARPGThreatRegistry* Registry = World->GetSubsystem<UARPGThreatRegistry>())
		{
			Registry->NotifyTargetChanged(OldTarget, NewTarget);
		}
	}
}

void UARPGPerceptionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// An NPC that dies or is streamed out stops threatening whoever it was after.
	// Without this its count would never come back down and the player's weapon
	// would stay drawn forever.
	PublishTargetChange(Target, nullptr);
	Target = nullptr;

	Super::EndPlay(EndPlayReason);
}

bool UARPGPerceptionComponent::IsAlive(const AActor* Candidate)
{
	if (!Candidate)
	{
		return false;
	}

	const UAbilitySystemComponent* ASC =
		UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Candidate);

	// No ability system means nothing that can die -- a destructible, a scripted
	// prop. Those are legitimate targets, so absence is not death.
	return !ASC || !ASC->HasMatchingGameplayTag(TAG_State_Dead);
}

UBlackboardComponent* UARPGPerceptionComponent::GetBlackboard() const
{
	// The component may live on the controller or the pawn; both need to reach
	// the same blackboard, and only the controller has one.
	if (AAIController* Controller = Cast<AAIController>(GetOwner()))
	{
		return Controller->GetBlackboardComponent();
	}

	if (const APawn* Pawn = Cast<APawn>(GetOwner()))
	{
		if (AAIController* Controller = Cast<AAIController>(Pawn->GetController()))
		{
			return Controller->GetBlackboardComponent();
		}
	}

	return nullptr;
}

void UARPGPerceptionComponent::WriteBlackboard()
{
	UBlackboardComponent* Blackboard = GetBlackboard();
	if (!Blackboard)
	{
		return;
	}

	Blackboard->SetValueAsObject(ARPGBlackboard::TargetActor, Target);
	Blackboard->SetValueAsBool(ARPGBlackboard::IsAlerted, bAlerted);

	// Only written when there IS one. Clearing it on target loss would erase the
	// one piece of information the NPC still needs -- where to go and look.
	if (!LastKnownLocation.IsNearlyZero())
	{
		Blackboard->SetValueAsVector(ARPGBlackboard::LastKnownLocation, LastKnownLocation);
	}
}

bool UARPGPerceptionComponent::HasLineOfSight(const AActor* Candidate) const
{
	const AActor* Self = GetOwner();
	const UWorld* World = GetWorld();
	if (!Self || !Candidate || !World)
	{
		return false;
	}

	const FVector Eye = Self->GetActorLocation() + FVector::UpVector * EyeHeight;
	const FVector TargetEye = Candidate->GetActorLocation() + FVector::UpVector * EyeHeight;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ARPGPerceptionLOS), /*bTraceComplex=*/false);
	Params.AddIgnoredActor(Self);
	Params.AddIgnoredActor(Candidate);

	// Eye to eye rather than centre to centre, so crouching behind cover works
	// and a knee-high rock does not blind the NPC.
	FHitResult Hit;
	return !World->LineTraceSingleByChannel(Hit, Eye, TargetEye, ECC_Visibility, Params);
}

bool UARPGPerceptionComponent::CanSee(AActor* Candidate, bool bApplyFOV) const
{
	const AActor* Self = GetOwner();
	if (!Self || !Candidate)
	{
		return false;
	}

	const FVector ToTarget = Candidate->GetActorLocation() - Self->GetActorLocation();
	if (ToTarget.SizeSquared() > FMath::Square(DetectionRange))
	{
		return false;
	}

	// The cone gates ACQUISITION only -- see the class comment. Callers pass
	// false once a target is already held.
	if (bApplyFOV && FOVAngle < 360.f)
	{
		const FVector Facing = Self->GetActorForwardVector().GetSafeNormal2D();
		const FVector ToTargetFlat = ToTarget.GetSafeNormal2D();

		const float HalfAngleCos = FMath::Cos(FMath::DegreesToRadians(FOVAngle * 0.5f));
		if (FVector::DotProduct(Facing, ToTargetFlat) < HalfAngleCos)
		{
			return false;
		}
	}

	return !bRequireLineOfSight || HasLineOfSight(Candidate);
}

bool UARPGPerceptionComponent::CanHear(AActor* Candidate) const
{
	const AActor* Self = GetOwner();
	if (!Self || !Candidate)
	{
		return false;
	}

	const UARPGNoiseComponent* Noise = Candidate->FindComponentByClass<UARPGNoiseComponent>();
	if (!Noise)
	{
		// Silent by construction. A candidate with no noise component is simply
		// invisible to this channel rather than infinitely quiet or infinitely
		// loud -- it falls back to sight alone.
		return false;
	}

	const float Distance = FVector::Dist(Self->GetActorLocation(), Candidate->GetActorLocation());
	return Distance <= Noise->GetCurrentNoiseRadius();
}

void UARPGPerceptionComponent::GatherCandidates(TArray<AActor*>& OutCandidates) const
{
	const AActor* Self = GetOwner();
	const UWorld* World = GetWorld();
	if (!Self || !World)
	{
		return;
	}

	// The overlap uses the DETECTION range, but hearing can exceed it -- a
	// sprinting player is audible from further away than they are visible. So the
	// query radius is the larger of the two bounds we could care about; the
	// per-sense checks then decide properly.
	const float QueryRadius = FMath::Max(DetectionRange, 5000.f);

	TArray<FOverlapResult> Overlaps;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ARPGPerceptionScan), /*bTraceComplex=*/false);
	Params.AddIgnoredActor(Self);

	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_Pawn);

	World->OverlapMultiByObjectType(Overlaps, Self->GetActorLocation(), FQuat::Identity,
		ObjectParams, FCollisionShape::MakeSphere(QueryRadius), Params);

	for (const FOverlapResult& Result : Overlaps)
	{
		AActor* Candidate = Result.GetActor();
		if (!Candidate || Candidate == Self)
		{
			continue;
		}

		// Faction, through the same library the hitbox filters with, so what an
		// NPC will fight and what it can damage never disagree.
		if (!UARPGCombatLibrary::CanDamage(const_cast<AActor*>(Self), Candidate))
		{
			continue;
		}

		// Corpses are not targets. Nothing granted State.Dead until now, so this
		// filter would have done nothing -- and NPCs stood around swinging at
		// bodies, which is exactly what it is here to stop.
		if (!IsAlive(Candidate))
		{
			continue;
		}

		OutCandidates.AddUnique(Candidate);
	}
}

void UARPGPerceptionComponent::Scan(float DeltaTime)
{
	// --- Retention -----------------------------------------------------------
	if (Target)
	{
		// A target that dies is released immediately rather than being held for
		// the memory window: there is nothing left to look for.
		if (!IsAlive(Target))
		{
			PublishTargetChange(Target, nullptr);
			Target = nullptr;
			bAlerted = false;
			MemoryTimer = 0.f;
			OnTargetLost.Broadcast();
			WriteBlackboard();
			return;
		}

		// No FOV: a target already engaged is not lost by turning away.
		const bool bStillPerceived = CanSee(Target, /*bApplyFOV=*/false) || CanHear(Target);

		if (bStillPerceived)
		{
			MemoryTimer = MemoryDuration;
			LastKnownLocation = Target->GetActorLocation();
			WriteBlackboard();
			return;
		}

		MemoryTimer -= DeltaTime;
		if (MemoryTimer > 0.f)
		{
			// Held, but not re-snapshotted: the NPC remembers where it last
			// actually saw them, which is the whole point of a memory window.
			WriteBlackboard();
			return;
		}

		PublishTargetChange(Target, nullptr);
		Target = nullptr;

		// Cleared with the target. This was set true on the first acquisition and
		// never reset, so IsAlerted was a one-way latch and any behaviour-tree
		// branch on it stayed taken for the rest of the NPC's life.
		bAlerted = false;

		OnTargetLost.Broadcast();
		WriteBlackboard();
		return;
	}

	// --- Acquisition ---------------------------------------------------------
	TArray<AActor*> Candidates;
	GatherCandidates(Candidates);

	AActor* Best = nullptr;
	float BestDistanceSq = TNumericLimits<float>::Max();

	for (AActor* Candidate : Candidates)
	{
		if (!CanSee(Candidate, /*bApplyFOV=*/true) && !CanHear(Candidate))
		{
			continue;
		}

		const float DistanceSq =
			FVector::DistSquared(GetOwner()->GetActorLocation(), Candidate->GetActorLocation());
		if (DistanceSq < BestDistanceSq)
		{
			BestDistanceSq = DistanceSq;
			Best = Candidate;
		}
	}

	if (Best)
	{
		SetTarget(Best);
	}
}

void UARPGPerceptionComponent::SetTarget(AActor* NewTarget)
{
	if (Target == NewTarget)
	{
		return;
	}

	PublishTargetChange(Target, NewTarget);
	Target = NewTarget;

	if (Target)
	{
		bAlerted = true;
		MemoryTimer = MemoryDuration;
		LastKnownLocation = Target->GetActorLocation();
		OnTargetAcquired.Broadcast(Target);
	}
	else
	{
		OnTargetLost.Broadcast();
	}

	WriteBlackboard();
}

void UARPGPerceptionComponent::NotifyDamagedBy(AActor* Attacker, FVector FromLocation)
{
	if (!Attacker)
	{
		return;
	}

	// Already fighting someone. A hit from a third party does not pull the NPC
	// off its current target -- otherwise a crowd fight becomes every NPC
	// swapping targets each time they are grazed.
	if (Target && Target != Attacker)
	{
		return;
	}

	// Being struck by someone already perceived just reaffirms them, whatever
	// the reaction setting says.
	const bool bAlreadyPerceived = Target == Attacker
		|| CanSee(Attacker, /*bApplyFOV=*/false)
		|| CanHear(Attacker);

	if (bAlreadyPerceived || DamageReaction == EARPGDamageReaction::Aggro)
	{
		// Bypasses range, cone and line of sight entirely: getting hit reveals
		// the attacker whether or not the NPC had spotted them.
		SetTarget(Attacker);
		return;
	}

	// Investigate: NO target and NO alert. The NPC walks over to look, and only
	// genuinely engages if its own senses then find something -- which is what
	// makes a stealth approach survivable after a first hit.
	LastKnownLocation = FromLocation;

	if (UBlackboardComponent* Blackboard = GetBlackboard())
	{
		Blackboard->SetValueAsVector(ARPGBlackboard::InvestigateLocation, FromLocation);
	}

	OnInvestigate.Broadcast(FromLocation);

	UE_LOG(LogARPGAI, Verbose, TEXT("%s was struck from %s and is investigating."),
		*GetNameSafe(GetOwner()), *FromLocation.ToCompactString());
}

void UARPGPerceptionComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// DeltaTime here is already the interval's worth of real seconds, because the
	// component tick interval is what paces this -- so memory counts down in real
	// seconds without an accumulator of our own.
	Scan(DeltaTime);
}
