// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGFuelComponent.h"
#include "ARPGSpreadSubsystem.h"
#include "ARPGWorld.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Net/UnrealNetwork.h"

UARPGFuelComponent::UARPGFuelComponent()
{
	// NO TICK. The subsystem drives this on the spread tick, because it is the
	// only thing that knows whether the cells under the object are alight -- and
	// asking the field once per object per frame, when the field itself only
	// advances a few times a second, would be work to arrive at last tick's
	// answer.
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UARPGFuelComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UARPGFuelComponent, FuelRemaining);
}

void UARPGFuelComponent::BeginPlay()
{
	Super::BeginPlay();

	// NEGATIVE MEANS UNSET, so an object placed in a level starts whole while one
	// that has already burned partway -- a level loaded from a save, an actor a
	// designer authored as scorched -- keeps whatever it was given.
	if (FuelRemaining < 0.f)
	{
		FuelRemaining = FuelSeconds;
	}

	PublishChar();

	if (!MediumTag.IsValid())
	{
		UE_LOG(LogARPGWorld, Warning,
			TEXT("'%s' is fuel for nothing -- it names no medium, so nothing will ever burn it."),
			*GetNameSafe(GetOwner()));
		return;
	}

	if (UARPGSpreadSubsystem* Spread =
			GetWorld() ? GetWorld()->GetSubsystem<UARPGSpreadSubsystem>() : nullptr)
	{
		Spread->RegisterFuel(this);
	}
}

void UARPGFuelComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UARPGSpreadSubsystem* Spread =
			GetWorld() ? GetWorld()->GetSubsystem<UARPGSpreadSubsystem>() : nullptr)
	{
		Spread->UnregisterFuel(this);
	}

	Super::EndPlay(EndPlayReason);
}

float UARPGFuelComponent::GetCharred() const
{
	if (FuelSeconds <= 0.f)
	{
		return 0.f;
	}

	return FMath::Clamp(1.f - FuelRemaining / FuelSeconds, 0.f, 1.f);
}

float UARPGFuelComponent::Burn(float DeltaTime, float Intensity)
{
	// A FIRE LICKING AT SOMETHING IS NOT A THING BURNING. Without the floor an
	// object sitting in the dying embers of a grass fire is eaten a hundredth at
	// a time until nothing is left, which reads as rot rather than as fire.
	bAlight = Intensity >= CatchThreshold && FuelRemaining > 0.f && DeltaTime > 0.f;

	if (!bAlight)
	{
		// AND THIS IS THE HALF-BURNT CRATE. Nothing here restores fuel, resets a
		// timer or decays anything -- the burn simply stops where it stopped, and
		// what is left is left. Quench a fire midway and the object keeps the
		// state it was in; relight it and it carries on from there.
		return 0.f;
	}

	const float Spent = FMath::Min(FuelRemaining, DeltaTime);
	FuelRemaining -= Spent;

	PublishChar();

	if (FuelRemaining <= 0.f)
	{
		FuelRemaining = 0.f;
		bAlight = false;

		// SPENT IS NOT DESTROYED. What that means differs per object and this
		// cannot know it: a log becomes charcoal, a rope parts, a barricade
		// collapses, a crate spills what was inside.
		OnSpent.Broadcast(this);

		UE_LOG(LogARPGWorld, Verbose, TEXT("'%s' has burned out."), *GetNameSafe(GetOwner()));
	}

	// WHAT IT GIVES BACK, in proportion to how much of the step it could sustain.
	// An object down to its last half-second does not feed a full step of fire,
	// which is what makes a woodpile fade rather than stop dead.
	return Output * (Spent / FMath::Max(KINDA_SMALL_NUMBER, DeltaTime));
}

void UARPGFuelComponent::Replenish(float Seconds)
{
	FuelRemaining = FMath::Clamp(FuelRemaining + Seconds, 0.f, FuelSeconds);
	PublishChar();
}

void UARPGFuelComponent::OnRep_Fuel()
{
	// The char is the only thing a client does with this, and it does it here --
	// the burn itself is server state and arrives as a number rather than as a
	// simulation the client repeats.
	PublishChar();
}

void UARPGFuelComponent::PublishChar()
{
	if (CharParameterIndex < 0)
	{
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	const float Charred = GetCharred();

	// CUSTOM PRIMITIVE DATA rather than a dynamic material instance. One float per
	// object, and a level full of crates each with its own MID is an allocation
	// and a broken batch apiece -- for a number the shader could have read from
	// the instance all along.
	Owner->ForEachComponent<UPrimitiveComponent>(/*bIncludeFromChildActors=*/true,
		[this, Charred](UPrimitiveComponent* Primitive)
		{
			Primitive->SetCustomPrimitiveDataFloat(CharParameterIndex, Charred);
		});
}
