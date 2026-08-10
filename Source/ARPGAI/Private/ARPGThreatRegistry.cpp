// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGThreatRegistry.h"
#include "GameFramework/Actor.h"

bool UARPGThreatRegistry::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game
		|| WorldType == EWorldType::PIE
		|| WorldType == EWorldType::GamePreview;
}

void UARPGThreatRegistry::NotifyTargetChanged(AActor* OldTarget, AActor* NewTarget)
{
	if (OldTarget == NewTarget)
	{
		return;
	}

	if (OldTarget)
	{
		const FObjectKey Key(OldTarget);
		if (int32* Count = ThreatCounts.Find(Key))
		{
			// Removed at zero rather than left behind: the map should describe
			// what is happening now, not everything that ever has.
			if (--(*Count) <= 0)
			{
				ThreatCounts.Remove(Key);
			}
		}
	}

	if (NewTarget)
	{
		++ThreatCounts.FindOrAdd(FObjectKey(NewTarget), 0);
	}
}

int32 UARPGThreatRegistry::GetThreatCount(const AActor* Actor) const
{
	if (!Actor)
	{
		return 0;
	}

	const int32* Count = ThreatCounts.Find(FObjectKey(Actor));
	return Count ? *Count : 0;
}

bool UARPGThreatRegistry::IsTargeted(const AActor* Actor) const
{
	return GetThreatCount(Actor) > 0;
}
