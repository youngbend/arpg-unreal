// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGReservoirVolumeComponent.h"
#include "ARPGWorld.h"
#include "GameFramework/Actor.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"

namespace
{
	/**
	 * How far under the reported surface the containment probe sits.
	 *
	 * ContainsPoint's vertical test is "at or below the waterline", so probing AT
	 * the waterline is an equality between two floats that were computed
	 * differently. A centimetre under is unambiguously in the water and is far
	 * below anything a bank could be mistaken for.
	 */
	constexpr float WaterProbeDepth = 1.f;

	/** Enough halvings to land the bank within a centimetre or so of a spell. */
	constexpr int32 EdgeSearchSteps = 8;
}

UARPGReservoirVolumeComponent::UARPGReservoirVolumeComponent()
{
	// What being a reservoir MEANS, as defaults rather than as a checklist every
	// designer has to remember: bottomless, damping, conductive, and something you
	// are standing in rather than something thrown at you.
	bReservoir = true;
	bAmbientSource = true;
	Absorption = 0.9f;
	Conductivity = 0.85f;
}

bool UARPGReservoirVolumeComponent::IsWaterAt(const FVector2D& Point) const
{
	const FVector Column(Point.X, Point.Y, 0.f);
	const float Surface = GetSurfaceHeightAt(Column);

	return ContainsPoint(FVector(Point.X, Point.Y, Surface - WaterProbeDepth));
}

double UARPGReservoirVolumeComponent::MarchToEdge(const FVector2D& Centre,
	const FVector2D& Direction, double MaxDistance) const
{
	// The whole reach is water: nothing to search for, the patch is unclipped
	// here. The common case for a shard landing mid-river.
	if (IsWaterAt(Centre + Direction * MaxDistance))
	{
		return MaxDistance;
	}

	// Otherwise the bank is somewhere between. Bisection rather than a fixed step
	// walk: a spell's radius is metres and a bank wants centimetre precision, and
	// halving gets there in eight probes instead of hundreds.
	double Inside = 0.0;
	double Outside = MaxDistance;

	for (int32 Step = 0; Step < EdgeSearchSteps; ++Step)
	{
		const double Middle = (Inside + Outside) * 0.5;

		if (IsWaterAt(Centre + Direction * Middle))
		{
			Inside = Middle;
		}
		else
		{
			Outside = Middle;
		}
	}

	return Inside;
}

TArray<FVector2D> UARPGReservoirVolumeComponent::GetSurfaceFootprint(const FVector2D& Centre,
	double Radius) const
{
	TArray<FVector2D> Footprint;

	// The contact itself has to be in the water. A shard that struck the bank
	// beside a river freezes nothing, which is the answer a box would have got
	// wrong and the reason containment is asked rather than assumed.
	if (Radius <= 0.0 || !IsWaterAt(Centre))
	{
		return Footprint;
	}

	const int32 Segments = FMath::Clamp(FootprintSegments, 6, 64);
	Footprint.Reserve(Segments);

	// A STAR AROUND THE CONTACT, not a disc. Each spoke stops where the water
	// does, so a shard landing at the edge of a river freezes a patch that hugs
	// the bank instead of a circle half of which is over dry ground.
	for (int32 Index = 0; Index < Segments; ++Index)
	{
		const double Angle = 2.0 * PI * Index / Segments;
		const FVector2D Direction(FMath::Cos(Angle), FMath::Sin(Angle));

		Footprint.Add(Centre + Direction * MarchToEdge(Centre, Direction, Radius));
	}

	return Footprint;
}

float UARPGReservoirVolumeComponent::GetSurfaceBedAt(const FVector2D& At) const
{
	// The floor of the channel, which for a box is simply its underside. A slab
	// too heavy to float comes to rest here.
	if (!OverlapSource)
	{
		return 0.f;
	}

	const FBoxSphereBounds ShapeBounds = OverlapSource->Bounds;
	return static_cast<float>(ShapeBounds.Origin.Z - ShapeBounds.BoxExtent.Z);
}

float UARPGReservoirVolumeComponent::GetSurfaceLevelAt(const FVector2D& At) const
{
	return GetSurfaceHeightAt(FVector(At.X, At.Y, 0.f));
}

bool UARPGReservoirVolumeComponent::ConsumeSurfaceArea(double Area)
{
	// A RIVER DOES NOT RUN OUT. Consume already refuses to spend a reservoir's
	// energy and there is a test named for it; freezing was the one path that
	// took area off a body it had just agreed was bottomless. Nothing to take,
	// and never used up.
	return false;
}

// ---------------------------------------------------------------------------
// Water plugin
// ---------------------------------------------------------------------------

UARPGWaterBodyVolumeComponent::UARPGWaterBodyVolumeComponent()
{
	// Nothing to add: ticking, the reservoir defaults and the replication rule all
	// come down from the parents. The class exists for its two overrides.
}

void UARPGWaterBodyVolumeComponent::BeginPlay()
{
	// BEFORE Super, which starts the ambient and immersion polling -- both of
	// which ask ContainsPoint, and both of which would spend their first ticks
	// answering from a null body.
	ResolveWaterBody();

	Super::BeginPlay();
}

void UARPGWaterBodyVolumeComponent::ResolveWaterBody()
{
	if (WaterBody)
	{
		return;
	}

	if (const AWaterBody* Body = Cast<AWaterBody>(GetOwner()))
	{
		WaterBody = Body->GetWaterBodyComponent();
	}

	if (!WaterBody)
	{
		WaterBody = GetOwner() ? GetOwner()->FindComponentByClass<UWaterBodyComponent>() : nullptr;
	}

	if (!WaterBody)
	{
		UE_LOG(LogARPGWorld, Warning,
			TEXT("'%s' has a water body volume but no water body to read, so it falls back to "
			     "its collider's bounds -- which for anything that bends covers the banks too."),
			*GetNameSafe(GetOwner()));
	}
}

bool UARPGWaterBodyVolumeComponent::ContainsPoint(FVector WorldPoint, float BelowReach) const
{
	if (!WaterBody)
	{
		return Super::ContainsPoint(WorldPoint, BelowReach);
	}

	// The SAME query the plugin's own buoyancy runs, so what floats and what is
	// wet agree by construction rather than by two systems being tuned to match.
	const EWaterBodyQueryFlags Flags =
		EWaterBodyQueryFlags::ComputeLocation | EWaterBodyQueryFlags::ComputeImmersionDepth;

	const FWaterBodyQueryResult Result =
		WaterBody->QueryWaterInfoClosestToWorldLocation(WorldPoint, Flags);

	if (Result.IsInExclusionVolume())
	{
		return false;
	}

	// BelowReach is the querying thing's own reach downward -- feet rather than
	// centre -- so it raises the point that has to be under the surface.
	return Result.GetImmersionDepth() >= -BelowReach;
}

FVector2D UARPGWaterBodyVolumeComponent::GetSurfaceFlowAt(const FVector2D& At) const
{
	if (!WaterBody)
	{
		return FVector2D::ZeroVector;
	}

	const FVector Probe(At.X, At.Y, GetSurfaceHeightAt(FVector(At.X, At.Y, 0.f)));

	const FWaterBodyQueryResult Result = WaterBody->QueryWaterInfoClosestToWorldLocation(
		Probe, EWaterBodyQueryFlags::ComputeLocation | EWaterBodyQueryFlags::ComputeVelocity);

	// FLAT. Whatever vertical component the flow has is the plugin describing a
	// waterfall, and a floe riding one is not a thing this models -- it drifts on
	// the surface or it does not drift.
	const FVector Velocity = Result.GetVelocity();
	return FVector2D(Velocity.X, Velocity.Y);
}

float UARPGWaterBodyVolumeComponent::GetSurfaceHeightAt(FVector WorldPoint) const
{
	if (!WaterBody)
	{
		return Super::GetSurfaceHeightAt(WorldPoint);
	}

	// Per-location, which is the entire point: a river runs downhill and one
	// authored offset cannot say so.
	const FWaterBodyQueryResult Result = WaterBody->QueryWaterInfoClosestToWorldLocation(
		WorldPoint, EWaterBodyQueryFlags::ComputeLocation);

	return static_cast<float>(Result.GetWaterSurfaceLocation().Z);
}
