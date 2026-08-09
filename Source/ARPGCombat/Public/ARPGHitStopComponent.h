// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ARPGHitStopComponent.generated.h"

class UAbilitySystemComponent;

/**
 * The brief animation freeze on a landed hit. Port of Godot's hit-stop,
 * deferred out of phase 3 with the VFX manager.
 *
 * A few frames of held pose is what makes a heavy attack land rather than pass
 * through. It is entirely presentational -- no attribute changes, no ability
 * blocking, and the character keeps MOVING throughout. Only the animation
 * stops, which is the whole trick: freezing the character in place as well
 * would be a stagger, and staggers are what poise is for.
 *
 * **Freezes BOTH parties**, since the attacker's swing has to catch on
 * something for the impact to read as a collision rather than a swing that
 * happened to be near a target.
 *
 * **Stacking hits take the LONGEST REMAINING rather than summing.** A multi-hit
 * exchange between two characters trading blows would otherwise pile up into a
 * lockup measured in whole seconds, which reads as a hitch rather than as
 * impact -- and the hitch gets worse exactly when the fight gets busiest.
 *
 * The Godot version needed an AnimationTree time-scale sweep plus a separate
 * pose-freeze node to hold the clips that had no time scale of their own.
 * `GlobalAnimRateScale` does the same job in one number: the mesh keeps
 * evaluating and applying its pose, so it never snaps to a reference pose, and
 * nothing plays on underneath to jump forward when the hold lifts.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCOMBAT_API UARPGHitStopComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGHitStopComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * Freezes this actor's animation for Duration seconds, on every machine.
	 *
	 * Server-authoritative entry point: call it on the server and every client
	 * sees the same freeze. Harmless to call on a client in standalone, where
	 * there is only one machine to convince.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Hit Stop")
	void ApplyHitStop(float Duration);

	/** Both parties, in one call. Safe with either side null or unequipped. */
	static void ApplyToPair(AActor* Attacker, AActor* Target, float Duration);

	UFUNCTION(BlueprintPure, Category = "ARPG|Hit Stop")
	bool IsFrozen() const { return Remaining > 0.f; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Hit Stop")
	float GetRemaining() const { return Remaining; }

	/**
	 * Longest freeze a single hit may ask for.
	 *
	 * A clamp rather than trust, because the duration is authored per attack and
	 * a slipped decimal point is a character frozen for ten seconds with no
	 * error to say why.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Hit Stop", meta = (ClampMin = "0.0"))
	float MaxDuration = 0.5f;

protected:
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastHitStop(float Duration);

	/** Sets the mesh's animation rate, or restores it. */
	void SetFrozen(bool bFrozen);

	/** Dead characters are never frozen; the death animation must play out. */
	bool IsDead() const;

private:
	float Remaining = 0.f;

	/**
	 * What the mesh's rate was before the freeze, restored on release.
	 *
	 * Captured on the FIRST freeze of a stretch only. Re-capturing on a
	 * re-entrant hit would snapshot the already-zeroed rate and restore the
	 * character to a permanent standstill -- the exact bug the Godot version
	 * guards against by not re-snapshotting while frozen.
	 */
	float PreviousAnimRate = 1.f;
};
