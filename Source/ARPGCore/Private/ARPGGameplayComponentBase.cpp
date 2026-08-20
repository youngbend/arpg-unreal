// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGGameplayComponentBase.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "GameFramework/Actor.h"

UAbilitySystemComponent* UARPGGameplayComponentBase::ResolveASC(const AActor* Actor)
{
	// const_cast because GetAbilitySystemComponentFromActor takes a mutable
	// actor while every caller here is asking a const question about one. The
	// lookup itself does not mutate the actor.
	return Actor
		? UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(const_cast<AActor*>(Actor))
		: nullptr;
}

UAbilitySystemComponent* UARPGGameplayComponentBase::GetASC() const
{
	if (!CachedASC)
	{
		CachedASC = ResolveASC(GetOwner());
	}
	return CachedASC;
}

bool UARPGGameplayComponentBase::HasAuthority() const
{
	const AActor* Owner = GetOwner();
	return Owner && Owner->HasAuthority();
}
