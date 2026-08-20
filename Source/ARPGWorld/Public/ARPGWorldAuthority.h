// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UWorld;

namespace ARPGWorld
{
	/**
	 * True where this machine owns the world simulation.
	 *
	 * THE ONE COPY. All four elemental solvers asked this question and three of
	 * them answered it with a byte-identical private method carrying a different
	 * comment; the fourth delegated to a file-local helper the others could not
	 * reach. It lives here because two of the four are tickable and two are not,
	 * so no single base class can hold it for all of them.
	 *
	 * WHY IT MATTERS. A world subsystem ticks and receives calls on clients as
	 * well as the server, and none of these solvers replicate anything. An
	 * ungated client runs its own divergent copy of world state and then applies
	 * gameplay effects and consumes invincibility frames from it -- burning real
	 * i-frames against a hazard the server never agreed existed.
	 *
	 * A world with no net driver -- an automation fixture -- counts as
	 * authoritative, because there is nobody else to be.
	 */
	ARPGWORLD_API bool WorldHasAuthority(const UWorld* World);
}
