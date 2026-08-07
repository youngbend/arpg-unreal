// Copyright Epic Games, Inc. All Rights Reserved.

#include "arpgGameMode.h"
#include "ARPGPlayerState.h"

AarpgGameMode::AarpgGameMode()
{
	// The ASC hangs off the PlayerState (see ARPGPlayerState.h), so the game
	// mode has to hand out ours rather than the stock APlayerState -- otherwise
	// every pawn's InitAbilityActorInfo finds nothing and the whole ability
	// system is quietly absent.
	PlayerStateClass = AARPGPlayerState::StaticClass();
}
