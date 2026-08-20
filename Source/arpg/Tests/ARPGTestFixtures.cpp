// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGTestFixtures.h"

#if WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS

#include "Components/ActorComponent.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"

namespace ARPGTest
{
	FTestWorld::FTestWorld(bool bNotifyWorldSettings)
	{
		// FSpawnHelper::GetWorld() creates the world on first call, so this is
		// where it comes into existence -- already through
		// InitializeActorsForPlay, which FActorTestSpawner::CreateWorld does.
		World = &Spawner.GetWorld();

		World->BeginPlay();

		// Opt-in -- see FTestWorldBegunPlay for why this is not the default.
		if (bNotifyWorldSettings)
		{
			if (AWorldSettings* Settings = World->GetWorldSettings())
			{
				Settings->NotifyBeginPlay();
			}
		}
	}

	void FTestWorld::Advance(float Seconds, float Step)
	{
		for (float Elapsed = 0.f; Elapsed < Seconds; Elapsed += Step)
		{
			World->Tick(LEVELTICK_All, Step);
		}
	}

	void TickComponents(const TArray<UActorComponent*>& Components, float Seconds, float Step)
	{
		for (float Elapsed = 0.f; Elapsed < Seconds; Elapsed += Step)
		{
			for (UActorComponent* Component : Components)
			{
				Component->TickComponent(Step, LEVELTICK_All, nullptr);
			}
		}
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS
