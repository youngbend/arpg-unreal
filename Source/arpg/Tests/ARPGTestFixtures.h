// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS

#include "CoreMinimal.h"
#include "Components/ActorTestSpawner.h"

class UActorComponent;

/**
 * The one test world every ARPG automation test runs in.
 *
 * WHY THIS FILE EXISTS. Seventeen test files each opened their own
 * ARPG*TestUtils namespace containing their own copy of a struct named
 * FTestWorld -- the same world-create, world-context, InitializeActorsForPlay,
 * BeginPlay, tear-it-all-down-again sequence, pasted seventeen times. They had
 * already drifted into six variants, and the drift was not cosmetic:
 *
 *   - TWO of the seventeen called AWorldSettings::NotifyBeginPlay(). The other
 *     fifteen did not, and the comment explaining why it is needed existed only
 *     in those two. UWorld::BeginPlay routes through the game mode, and a world
 *     built by hand has none -- so it returns having started nothing, the world
 *     never reports having begun play, and every actor spawned afterwards is
 *     skipped too. Anything whose state is set up in BeginPlay -- a spell arming
 *     its reaction volume, a fuel component filling itself from FuelSeconds --
 *     was silently left at its defaults in fifteen test files.
 *   - TWO had an Advance() stepper, one of which never used it.
 *   - One passed DestroyWorld(false) where the rest passed a commented argument.
 *
 * That is the whole argument for one copy: a fix applied to a pasted fixture
 * reaches whichever paste the author happened to be looking at.
 *
 * THE LIFECYCLE IS THE ENGINE'S. FActorTestSpawner (CQTest, shipped in
 * Engine/Source/Developer/CQTest) creates and destroys the world, and its
 * teardown does what the hand-rolled destructor never did: routes EndPlay to
 * every actor and shuts down the net driver before destroying the world. The
 * missing EndPlay is exactly what made almost every test log "CleanupWorld
 * called on a world that has begun play, missing call to EndPlay".
 *
 * The member is still called World, and Advance still takes the same arguments,
 * so the call sites in the seventeen files did not have to change.
 */
namespace ARPGTest
{
	struct FTestWorld
	{
		/**
		 * @param bNotifyWorldSettings  also route BeginPlay through
		 *        AWorldSettings. See FTestWorldBegunPlay -- it is NOT the
		 *        default, because it changes what a spawned actor has already
		 *        done by the time a test looks at it.
		 */
		explicit FTestWorld(bool bNotifyWorldSettings = false);

		/** The world under test. Owned by the spawner, valid for this object's life. */
		UWorld* World = nullptr;

		/**
		 * Advances world time in small steps so timer-driven effects fire.
		 *
		 * Stepped rather than ticked once: a single large tick fires a periodic
		 * gameplay effect once no matter how many periods it spans.
		 */
		void Advance(float Seconds, float Step = 0.05f);

	private:
		/**
		 * Declared last so it is destroyed FIRST -- members are torn down in
		 * reverse declaration order, and nothing above may outlive the world.
		 */
		FActorTestSpawner Spawner;
	};

	/**
	 * A test world that also routes BeginPlay through AWorldSettings.
	 *
	 * UWorld::BeginPlay routes through the game mode, and a world built by hand
	 * has none -- so it returns having started nothing, the world never reports
	 * having begun play, and every actor spawned afterwards is skipped too.
	 * Anything whose state is set up in BeginPlay -- a spell arming its reaction
	 * volume, a fuel component filling itself from FuelSeconds -- is otherwise
	 * left at its defaults. This is what a game mode's StartPlay does.
	 *
	 * A SEPARATE TYPE RATHER THAN THE DEFAULT. Only the reaction and spread
	 * suites were written against it, and they are the two that need actors to
	 * have initialised themselves before the test looks. Turning it on
	 * everywhere changes what the fluid suite observes -- solids begin play and
	 * react before its setup has finished arranging them, and it starts counting
	 * two crusts where it means to count one. Which behaviour a suite wants is a
	 * real choice, so it is spelled out per suite rather than defaulted.
	 */
	struct FTestWorldBegunPlay : FTestWorld
	{
		FTestWorldBegunPlay() : FTestWorld(/*bNotifyWorldSettings=*/true) {}
	};

	/**
	 * Ticks specific components by hand.
	 *
	 * A world built this way does not register component ticks for actors
	 * spawned into it, so World::Tick alone leaves every timer in these
	 * components frozen -- which reads exactly like a blend-in that never
	 * completes or a meter that never drains. Driving them directly keeps the
	 * test measuring the components rather than the harness.
	 */
	void TickComponents(const TArray<UActorComponent*>& Components, float Seconds, float Step = 0.02f);
}

#endif // WITH_DEV_AUTOMATION_TESTS && ARPG_WITH_TESTS
