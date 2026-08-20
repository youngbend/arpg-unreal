// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGWorldAuthority.h"
#include "Engine/World.h"

namespace ARPGWorld
{
	bool WorldHasAuthority(const UWorld* World)
	{
		return !World || World->GetNetMode() != NM_Client;
	}
}
