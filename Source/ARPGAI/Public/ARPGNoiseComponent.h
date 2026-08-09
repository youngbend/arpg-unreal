// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ARPGNoiseComponent.generated.h"

/**
 * How loud this character currently is. Port of Godot's NoiseComponent.
 *
 * A POLLED RADIUS, not an event. Listeners ask "how far can this be heard right
 * now" whenever they need to know, rather than the noisemaker broadcasting into
 * a subscriber list. That is the right shape because noise here is a CONTINUOUS
 * property of how the character is moving -- sprinting is loud for as long as you
 * sprint -- and an event stream would have to synthesise a tick rate to
 * represent it.
 *
 * Deliberately not UE's UAISense_Hearing, which is event-based for exactly the
 * cases this is not: a footstep, a gunshot, a thrown rock.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGAI_API UARPGNoiseComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGNoiseComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Noise",
		meta = (ClampMin = "0.0"))
	float IdleRadius = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Noise",
		meta = (ClampMin = "0.0"))
	float WalkRadius = 400.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Noise",
		meta = (ClampMin = "0.0"))
	float SprintRadius = 1200.f;

	/**
	 * Applied while attacking, as a FLOOR rather than a replacement -- someone
	 * sprinting and swinging is at least as loud as sprinting.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Noise",
		meta = (ClampMin = "0.0"))
	float AttackRadius = 900.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Noise",
		meta = (ClampMin = "0.0"))
	float IdleSpeedThreshold = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Noise",
		meta = (ClampMin = "0.0"))
	float SprintSpeedThreshold = 500.f;

	/** Current audible radius, from speed and combat state. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Noise")
	float GetCurrentNoiseRadius() const;
};
