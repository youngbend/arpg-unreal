// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGWorldSettings.h"

const UARPGWorldSettings& UARPGWorldSettings::Get()
{
	const UARPGWorldSettings* Settings = GetDefault<UARPGWorldSettings>();
	check(Settings);
	return *Settings;
}
