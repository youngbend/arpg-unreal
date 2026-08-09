// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGHitStopComponent.h"
#include "ARPGGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"

UARPGHitStopComponent::UARPGHitStopComponent()
{
	// Ticks only while frozen. A component that exists to do nothing most of the
	// time should cost nothing most of the time.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;

	// Replicated so the multicast below has a channel to travel on.
	SetIsReplicatedByDefault(true);
}

void UARPGHitStopComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (Remaining <= 0.f)
	{
		return;
	}

	// UNDILATED time. The freeze is measured in real seconds, so a slow-motion
	// finisher does not also stretch every hit-stop inside it into a stall.
	const UWorld* World = GetWorld();
	const float RealDelta = World ? DeltaTime / FMath::Max(KINDA_SMALL_NUMBER,
		World->GetWorldSettings()->GetEffectiveTimeDilation()) : DeltaTime;

	Remaining -= RealDelta;
	if (Remaining <= 0.f)
	{
		Remaining = 0.f;
		SetFrozen(false);
		SetComponentTickEnabled(false);
	}
}

void UARPGHitStopComponent::ApplyHitStop(float Duration)
{
	if (Duration <= 0.f)
	{
		return;
	}

	MulticastHitStop(FMath::Min(Duration, MaxDuration));
}

void UARPGHitStopComponent::MulticastHitStop_Implementation(float Duration)
{
	// Never on a corpse: the death animation has to play out, and a freeze
	// landing during it holds the character mid-collapse until it lifts.
	if (Duration <= 0.f || IsDead())
	{
		return;
	}

	// The FIRST freeze of a stretch captures the rate. A re-entrant hit only
	// extends the timer -- see PreviousAnimRate.
	if (Remaining <= 0.f)
	{
		SetFrozen(true);
	}

	// LONGEST REMAINING, not the sum. Two hits landing together are one impact,
	// not two stacked pauses.
	Remaining = FMath::Max(Remaining, Duration);
	SetComponentTickEnabled(true);
}

void UARPGHitStopComponent::SetFrozen(bool bFrozen)
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	USkeletalMeshComponent* Mesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	if (!Mesh)
	{
		return;
	}

	if (bFrozen)
	{
		PreviousAnimRate = Mesh->GlobalAnimRateScale;
		Mesh->GlobalAnimRateScale = 0.f;
	}
	else
	{
		Mesh->GlobalAnimRateScale = PreviousAnimRate;
		PreviousAnimRate = 1.f;
	}
}

bool UARPGHitStopComponent::IsDead() const
{
	const UAbilitySystemComponent* ASC =
		UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetOwner());
	return ASC && ASC->HasMatchingGameplayTag(TAG_State_Dead);
}

void UARPGHitStopComponent::ApplyToPair(AActor* Attacker, AActor* Target, float Duration)
{
	if (Duration <= 0.f)
	{
		return;
	}

	// Both, because the attacker's swing has to catch on something. A target
	// that freezes alone reads as the target glitching, not as a hit landing.
	for (AActor* Actor : { Attacker, Target })
	{
		if (!Actor)
		{
			continue;
		}

		if (UARPGHitStopComponent* HitStop = Actor->FindComponentByClass<UARPGHitStopComponent>())
		{
			HitStop->ApplyHitStop(Duration);
		}
	}
}
