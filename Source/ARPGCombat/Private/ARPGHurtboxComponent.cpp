// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGHurtboxComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"

UARPGHurtboxComponent::UARPGHurtboxComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	SetIsReplicatedByDefault(false); // server-authoritative; nothing here is client-visible
}

void UARPGHurtboxComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (InvincibilityTimer > 0.f)
	{
		InvincibilityTimer = FMath::Max(0.f, InvincibilityTimer - DeltaTime);
	}
}

UAbilitySystemComponent* UARPGHurtboxComponent::GetAbilitySystemComponent() const
{
	if (!CachedASC)
	{
		// Resolved lazily rather than in BeginPlay: for a player pawn the ASC
		// lives on the PlayerState, which may not have replicated in yet when
		// this component begins play.
		CachedASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetOwner());
	}
	return CachedASC;
}

bool UARPGHurtboxComponent::TryConsumeHit()
{
	if (IsInvincible())
	{
		return false;
	}

	if (InvincibilityDuration > 0.f)
	{
		InvincibilityTimer = InvincibilityDuration;
	}

	return true;
}
