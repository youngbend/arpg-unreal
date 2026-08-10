// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGParryComponent.h"
#include "ARPGGameplayTags.h"
#include "AbilitySystemComponent.h"

UARPGParryComponent::UARPGParryComponent()
{
	// Idle until something opens a timer; see RefreshTickState.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	SetIsReplicatedByDefault(false); // guard state is resolved server-side
}

void UARPGParryComponent::RefreshTickState()
{
	const bool bNeedsTick = bBlocking
		|| ParryCooldownTimer > 0.f
		|| (bEmpowered && EmpoweredDuration > 0.f);

	SetComponentTickEnabled(bNeedsTick);
}

void UARPGParryComponent::BeginBlock()
{
	if (bBlocking)
	{
		return;
	}

	bBlocking = true;
	BlendInTimer = BlendInTime;

	// Re-pressing guard too soon raises it without a parry window. Otherwise
	// mashing the key would produce a continuous parry, which removes the timing
	// entirely.
	ParryTimer = (ParryCooldownTimer > 0.f) ? 0.f : ParryWindow;

	if (UAbilitySystemComponent* ASC = GetASC())
	{
		ASC->AddLooseGameplayTag(TAG_State_Blocking, 1, EGameplayTagReplicationState::TagOnly);
	}

	RefreshTickState();
	OnBlockStarted.Broadcast();
}

void UARPGParryComponent::EndBlock()
{
	if (!bBlocking)
	{
		return;
	}

	bBlocking = false;
	BlendInTimer = 0.f;
	ParryTimer = 0.f;
	ParryCooldownTimer = ParryCooldown;

	if (UAbilitySystemComponent* ASC = GetASC())
	{
		ASC->RemoveLooseGameplayTag(TAG_State_Blocking);
		ASC->RemoveLooseGameplayTag(TAG_State_Parrying);
	}

	RefreshTickState();
	OnBlockEnded.Broadcast();
}

EARPGInterceptResult UARPGParryComponent::PeekIntercept() const
{
	if (!bBlocking || BlendInTimer > 0.f)
	{
		return EARPGInterceptResult::None;
	}

	return ParryTimer > 0.f ? EARPGInterceptResult::Parried : EARPGInterceptResult::Blocked;
}

EARPGInterceptResult UARPGParryComponent::TryIntercept()
{
	if (!bBlocking)
	{
		return EARPGInterceptResult::None;
	}

	// Still raising the guard. Deliberately NOT even a block: the blend-in is
	// what makes committing to a parry a real decision rather than a reaction.
	if (BlendInTimer > 0.f)
	{
		return EARPGInterceptResult::None;
	}

	if (ParryTimer > 0.f)
	{
		// Consume the window, so one guard press yields at most one parry.
		ParryTimer = 0.f;
		bEmpowered = true;
		EmpoweredTimer = EmpoweredDuration;
		RefreshTickState();

		OnIntercepted.Broadcast(EARPGInterceptResult::Parried);
		return EARPGInterceptResult::Parried;
	}

	OnIntercepted.Broadcast(EARPGInterceptResult::Blocked);
	return EARPGInterceptResult::Blocked;
}

bool UARPGParryComponent::ConsumeEmpowered()
{
	if (!bEmpowered)
	{
		return false;
	}

	bEmpowered = false;
	EmpoweredTimer = 0.f;
	RefreshTickState();
	return true;
}

void UARPGParryComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (ParryCooldownTimer > 0.f)
	{
		ParryCooldownTimer = FMath::Max(0.f, ParryCooldownTimer - DeltaTime);
	}

	if (bEmpowered && EmpoweredDuration > 0.f)
	{
		EmpoweredTimer = FMath::Max(0.f, EmpoweredTimer - DeltaTime);
		if (EmpoweredTimer <= 0.f)
		{
			bEmpowered = false;
		}
	}

	if (!bBlocking)
	{
		// Blocking is the only state that needs a per-frame tick indefinitely;
		// the cooldown and empowered timers above run down and stop.
		RefreshTickState();
		return;
	}

	if (BlendInTimer > 0.f)
	{
		BlendInTimer = FMath::Max(0.f, BlendInTimer - DeltaTime);

		// The parry window opens the instant the guard is fully up.
		if (BlendInTimer <= 0.f && ParryTimer > 0.f)
		{
			if (UAbilitySystemComponent* ASC = GetASC())
			{
				ASC->AddLooseGameplayTag(TAG_State_Parrying, 1, EGameplayTagReplicationState::TagOnly);
			}
		}
		return;
	}

	if (ParryTimer > 0.f)
	{
		ParryTimer = FMath::Max(0.f, ParryTimer - DeltaTime);
		if (ParryTimer <= 0.f)
		{
			if (UAbilitySystemComponent* ASC = GetASC())
			{
				ASC->RemoveLooseGameplayTag(TAG_State_Parrying);
			}
		}
	}
}
