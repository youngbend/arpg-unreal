// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGStatusResistanceComponent.h"

UARPGStatusResistanceComponent::UARPGStatusResistanceComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false); // consulted only on the server, at application time
}
