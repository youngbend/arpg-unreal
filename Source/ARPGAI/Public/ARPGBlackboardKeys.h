// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The blackboard keys every ARPG behaviour tree is expected to declare.
 *
 * Named once here rather than typed into each node's default, because a
 * blackboard key is a contract between the tree asset and the C++ that reads it
 * -- and a typo in either produces a node that silently does nothing rather than
 * failing. Nodes still expose their key as an editable selector; these are the
 * defaults that make a tree work without configuring every node by hand.
 */
namespace ARPGBlackboard
{
	/** The actor this NPC is currently fighting. Object key. */
	ARPGAI_API extern const FName TargetActor;

	/**
	 * Where the target was last actually perceived. Vector key.
	 *
	 * Separate from the target itself because the two have different lifetimes:
	 * the target goes null the moment perception drops it, and this is what the
	 * NPC still has to go and look at.
	 */
	ARPGAI_API extern const FName LastKnownLocation;

	/** Where the NPC started, for leash checks. Vector key. */
	ARPGAI_API extern const FName HomeLocation;

	/** Set once on first acquisition. Bool key. */
	ARPGAI_API extern const FName IsAlerted;

	/**
	 * A position to walk to and look around, with no target granted. Vector key.
	 *
	 * The investigate reaction writes this; a tree branch reads it to send the
	 * NPC over without ever claiming it knows who is there.
	 */
	ARPGAI_API extern const FName InvestigateLocation;
}
