// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGHurtboxComponent.h"
#include "ARPGGameplayTags.h"
#include "ARPGHurtboxRegistry.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

UARPGHurtboxComponent::UARPGHurtboxComponent()
{
	// Off until a window actually opens. There is one of these on every
	// damageable thing in the world and the timer it counts down is almost
	// always already zero.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	SetIsReplicatedByDefault(false); // server-authoritative; nothing here is client-visible
}

void UARPGHurtboxComponent::BeginPlay()
{
	Super::BeginPlay();

	if (const UWorld* World = GetWorld())
	{
		if (UARPGHurtboxRegistry* Registry = World->GetSubsystem<UARPGHurtboxRegistry>())
		{
			Registry->Register(GetOwner(), this);
		}
	}
}

void UARPGHurtboxComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (const UWorld* World = GetWorld())
	{
		if (UARPGHurtboxRegistry* Registry = World->GetSubsystem<UARPGHurtboxRegistry>())
		{
			Registry->Unregister(GetOwner());
		}
	}

	Super::EndPlay(EndPlayReason);
}

UARPGHurtboxComponent* UARPGHurtboxComponent::FindFor(const AActor* Actor)
{
	if (!Actor)
	{
		return nullptr;
	}

	UARPGHurtboxRegistry* Registry = nullptr;
	if (const UWorld* World = Actor->GetWorld())
	{
		Registry = World->GetSubsystem<UARPGHurtboxRegistry>();
	}

	if (Registry)
	{
		if (UARPGHurtboxComponent* Registered = Registry->Find(Actor))
		{
			return Registered;
		}
	}

	// A MISS IS NOT AN ANSWER. Registration happens in BeginPlay, and plenty of
	// legitimate cases reach here first: a component added at runtime, an actor
	// spawned into a world that has not begun play, an automation fixture. The
	// scan is the fallback, and what it finds is registered so this costs a walk
	// once rather than on every query.
	UARPGHurtboxComponent* Found =
		const_cast<AActor*>(Actor)->FindComponentByClass<UARPGHurtboxComponent>();

	if (Found && Registry)
	{
		Registry->Register(const_cast<AActor*>(Actor), Found);
	}

	return Found;
}

void UARPGHurtboxComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (InvincibilityTimer > 0.f)
	{
		InvincibilityTimer = FMath::Max(0.f, InvincibilityTimer - DeltaTime);
	}

	if (InvincibilityTimer <= 0.f)
	{
		SetComponentTickEnabled(false);
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

bool UARPGHurtboxComponent::IsInvincible() const
{
	if (bForceInvincible || InvincibilityTimer > 0.f)
	{
		return true;
	}

	const UAbilitySystemComponent* ASC = GetAbilitySystemComponent();
	return ASC && ASC->HasMatchingGameplayTag(TAG_State_Invulnerable);
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
		SetComponentTickEnabled(true);
	}

	return true;
}
