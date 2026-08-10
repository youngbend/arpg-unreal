// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGHurtboxRegistry.h"
#include "ARPGHurtboxComponent.h"
#include "GameFramework/Actor.h"

bool UARPGHurtboxRegistry::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Editor preview worlds have no gameplay to route, and creating the subsystem
	// there would keep a map alive for every thumbnail scene.
	return WorldType == EWorldType::Game
		|| WorldType == EWorldType::PIE
		|| WorldType == EWorldType::GamePreview;
}

void UARPGHurtboxRegistry::Register(AActor* Actor, UARPGHurtboxComponent* Hurtbox)
{
	if (Actor && Hurtbox)
	{
		Hurtboxes.Add(FObjectKey(Actor), Hurtbox);
	}
}

void UARPGHurtboxRegistry::Unregister(const AActor* Actor)
{
	if (Actor)
	{
		Hurtboxes.Remove(FObjectKey(Actor));
	}
}

UARPGHurtboxComponent* UARPGHurtboxRegistry::Find(const AActor* Actor)
{
	if (!Actor)
	{
		return nullptr;
	}

	const FObjectKey Key(Actor);
	if (TWeakObjectPtr<UARPGHurtboxComponent>* Found = Hurtboxes.Find(Key))
	{
		if (UARPGHurtboxComponent* Hurtbox = Found->Get())
		{
			return Hurtbox;
		}

		// The entry outlived its component -- an actor destroyed without EndPlay.
		// Dropped here rather than swept on a timer, so the cost falls on the one
		// query that notices rather than on every tick.
		Hurtboxes.Remove(Key);
	}

	return nullptr;
}
