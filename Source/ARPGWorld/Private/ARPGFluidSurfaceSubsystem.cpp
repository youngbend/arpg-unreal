// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGFluidSurfaceSubsystem.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGFluidBody.h"
#include "ARPGFluidDefinition.h"
#include "ARPGFluidGeometry.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicElement.h"
#include "ARPGWorld.h"
#include "ARPGWorldSettings.h"
#include "Engine/World.h"

void UARPGFluidSurfaceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Project settings fill in anything a test has not already assigned. These
	// fields are EditAnywhere on a UWorldSubsystem, which has no editing surface,
	// so outside the tests every one of them was null: no fluid definitions meant
	// no pool ever formed.
	const UARPGWorldSettings& Settings = UARPGWorldSettings::Get();

	if (Definitions.Num() == 0)
	{
		for (const TSoftObjectPtr<UARPGFluidDefinition>& Soft : Settings.FluidDefinitions)
		{
			if (UARPGFluidDefinition* Definition = Soft.LoadSynchronous())
			{
				Definitions.Add(Definition);
			}
			else if (!Soft.IsNull())
			{
				UE_LOG(LogARPGWorld, Warning,
					TEXT("Fluid definition '%s' from project settings failed to load."),
					*Soft.ToString());
			}
		}
	}

	if (!CombinationTable)
	{
		CombinationTable = Settings.CombinationTable.LoadSynchronous();
	}
}

bool UARPGFluidSurfaceSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

bool UARPGFluidSurfaceSubsystem::HasAuthority() const
{
	const UWorld* World = GetWorld();

	// Pools and solids are replicated actors owned by the server. A client
	// spawning its own would leave every puddle in the level doubled.
	return !World || World->GetNetMode() != NM_Client;
}

TStatId UARPGFluidSurfaceSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UARPGFluidSurfaceSubsystem, STATGROUP_Tickables);
}

UARPGFluidDefinition* UARPGFluidSurfaceSubsystem::FindDefinition(FGameplayTag ElementTag) const
{
	for (UARPGFluidDefinition* Definition : Definitions)
	{
		if (Definition && Definition->GetElementTag() == ElementTag)
		{
			return Definition;
		}
	}
	return nullptr;
}

UARPGSolidDefinition* UARPGFluidSurfaceSubsystem::FindSolidDefinition(FGameplayTag ElementTag) const
{
	for (UARPGSolidDefinition* Definition : Solids)
	{
		if (Definition && Definition->GetElementTag() == ElementTag)
		{
			return Definition;
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Depositing
// ---------------------------------------------------------------------------

AARPGFluidPool* UARPGFluidSurfaceSubsystem::Deposit(FVector WorldPosition, float Radius,
	FGameplayTag ElementTag)
{
	UARPGFluidDefinition* Definition = FindDefinition(ElementTag);
	if (!Definition)
	{
		return nullptr;
	}

	const TArray<FVector2D> Footprint = ARPGFluidGeometry::MakeCircle(
		FVector2D(WorldPosition.X, WorldPosition.Y), Radius);

	return DepositRing(Footprint, WorldPosition.Z, Definition);
}

AARPGFluidPool* UARPGFluidSurfaceSubsystem::DepositSwept(FVector From, FVector To, float Radius,
	FGameplayTag ElementTag)
{
	UARPGFluidDefinition* Definition = FindDefinition(ElementTag);
	if (!Definition)
	{
		return nullptr;
	}

	// The honest footprint of an elongated spell: a stadium, not a disc at each
	// end. Every forward-elongated discharge in this project sweeps.
	const TArray<FVector2D> Footprint = ARPGFluidGeometry::MakeStadium(
		FVector2D(From.X, From.Y), FVector2D(To.X, To.Y), Radius);

	return DepositRing(Footprint, FMath::Min(From.Z, To.Z), Definition);
}

AARPGFluidPool* UARPGFluidSurfaceSubsystem::DepositRing(const TArray<FVector2D>& Footprint,
	float GroundHeight, UARPGFluidDefinition* Definition)
{
	UWorld* World = GetWorld();
	if (!World || Footprint.Num() < 3)
	{
		return nullptr;
	}

	const FVector2D Centre = ARPGFluidGeometry::PolygonCentroid(Footprint);

	// MERGE rather than stack. Two puddles of the same thing overlapping are one
	// puddle; leaving them as separate bodies would double their ambient effect
	// and make the pair react twice to the same spell.
	for (int32 Index = Pools.Num() - 1; Index >= 0; --Index)
	{
		AARPGFluidPool* Pool = Pools[Index];
		if (!IsValid(Pool) || Pool->Definition != Definition)
		{
			continue;
		}

		const FBox2D PoolBounds = ARPGFluidGeometry::PolygonBounds(Pool->GetRing());
		const FBox2D Expanded = PoolBounds.ExpandBy(Definition->MergeDistance);

		if (!Expanded.IsInside(Centre))
		{
			continue;
		}

		Pool->SetRing(ARPGFluidGeometry::MergeRings(Pool->GetRing(), Footprint));
		return Pool;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AARPGFluidPool* Pool = World->SpawnActor<AARPGFluidPool>(
		AARPGFluidPool::StaticClass(),
		FTransform(FVector(Centre.X, Centre.Y, GroundHeight)), Params);

	if (!Pool)
	{
		return nullptr;
	}

	Pool->Setup(Definition, Footprint, GroundHeight);
	Pools.Add(Pool);

	return Pool;
}

// ---------------------------------------------------------------------------
// Solidifying
// ---------------------------------------------------------------------------

bool UARPGFluidSurfaceSubsystem::TrySolidify(UARPGElementalVolumeComponent* A,
	UARPGElementalVolumeComponent* B)
{
	if (!A || !B || !A->Element || !B->Element || !CombinationTable)
	{
		return false;
	}

	FGameplayTagContainer Pair;
	Pair.AddTag(A->Element->ElementTag);
	Pair.AddTag(B->Element->ElementTag);

	const UARPGMagicCombinationEntry* Entry =
		CombinationTable->ResolveEntry(Pair, EARPGCombinationScope::Surface);

	// A SURFACE-scope Solidify row, specifically. An ice shard meeting a water
	// JET is a collision that trades energy; the same shard meeting a PUDDLE
	// freezes part of its surface. One relationship, two physics, two rows.
	if (!Entry || Entry->Mode != EARPGReactionMode::Solidify || !Entry->Result)
	{
		return false;
	}

	// One side has to actually be a body lying on the ground -- that is what
	// there is to freeze.
	AARPGFluidPool* Pool = Cast<AARPGFluidPool>(A->GetOwner());
	UARPGElementalVolumeComponent* Agent = B;

	if (!Pool)
	{
		Pool = Cast<AARPGFluidPool>(B->GetOwner());
		Agent = A;
	}

	if (!Pool || !Agent->OverlapSource)
	{
		return false;
	}

	UARPGSolidDefinition* SolidDefinition = FindSolidDefinition(Entry->Result->ElementTag);
	if (!SolidDefinition)
	{
		UE_LOG(LogARPGWorld, Warning,
			TEXT("A Solidify row produces '%s', but no solid definition describes it, so "
			     "nothing was frozen."),
			*Entry->Result->ElementTag.ToString());
		return false;
	}

	// The agent's own footprint, so a big shard freezes more than a small one.
	const FBoxSphereBounds AgentBounds = Agent->OverlapSource->Bounds;
	const TArray<FVector2D> AgentRing = ARPGFluidGeometry::MakeCircle(
		FVector2D(AgentBounds.Origin.X, AgentBounds.Origin.Y),
		FMath::Max(AgentBounds.BoxExtent.X, AgentBounds.BoxExtent.Y));

	// THE OVERLAP, not the whole pool and not the whole shard. Freezing exactly
	// where the two met is the entire reason a body is a polygon rather than a
	// disc or a grid cell.
	TArray<FVector2D> FrozenRing;
	TArray<TArray<FVector2D>> Holes;
	ARPGFluidGeometry::IntersectWithHoles(Pool->GetRing(), AgentRing, FrozenRing, Holes);

	const double FrozenArea = ARPGFluidGeometry::PolygonArea(FrozenRing);
	if (FrozenArea < SolidDefinition->MinimumArea)
	{
		return false;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	const FVector2D Centre = ARPGFluidGeometry::PolygonCentroid(FrozenRing);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AARPGFluidSolid* Solid = World->SpawnActor<AARPGFluidSolid>(
		AARPGFluidSolid::StaticClass(),
		FTransform(FVector(Centre.X, Centre.Y, Pool->GetSurfaceHeight())), Params);

	if (!Solid)
	{
		return false;
	}

	Solid->Setup(SolidDefinition, FrozenRing, Pool->GetSurfaceHeight());

	// Only the largest hole is carried: a solid keeps its gaps, but tracking an
	// arbitrary set of them buys nothing a single dominant gap does not, and
	// each extra ring is another thing erosion has to keep clean.
	if (Holes.Num() > 0)
	{
		int32 Largest = 0;
		for (int32 Index = 1; Index < Holes.Num(); ++Index)
		{
			if (ARPGFluidGeometry::PolygonArea(Holes[Index])
				> ARPGFluidGeometry::PolygonArea(Holes[Largest]))
			{
				Largest = Index;
			}
		}
		Solid->HoleRing = Holes[Largest];
	}

	ActiveSolids.Add(Solid);

	// The fluid is genuinely USED UP. Subtracting the frozen region would leave
	// a hole, and a liquid flows back over a hole -- so the pool keeps its shape
	// and loses the area instead, which is what ShrinkToArea is for.
	const double Remaining = FMath::Max(0.0, ARPGFluidGeometry::PolygonArea(Pool->GetRing()) - FrozenArea);
	if (Remaining < Pool->Definition->MinimumArea)
	{
		Pools.Remove(Pool);
		Pool->Destroy();
	}
	else
	{
		Pool->SetRing(ARPGFluidGeometry::ShrinkToArea(Pool->GetRing(), Remaining));
	}

	// Spent freezing it.
	Agent->Consume(Agent->GetEnergy(), Entry->Result);

	UE_LOG(LogARPGWorld, Log, TEXT("Froze %.0f square units of '%s' into '%s'."),
		FrozenArea, *Pool->Definition->GetElementTag().ToString(),
		*SolidDefinition->GetElementTag().ToString());

	return true;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

AARPGFluidPool* UARPGFluidSurfaceSubsystem::FindPoolAt(FVector WorldPosition) const
{
	for (AARPGFluidPool* Pool : Pools)
	{
		if (IsValid(Pool) && Pool->ContainsPoint(WorldPosition))
		{
			return Pool;
		}
	}
	return nullptr;
}

bool UARPGFluidSurfaceSubsystem::IsCoveredBySolid(FVector WorldPosition) const
{
	for (const AARPGFluidSolid* Solid : ActiveSolids)
	{
		if (IsValid(Solid) && Solid->IsStandableAt(WorldPosition))
		{
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// Weather
// ---------------------------------------------------------------------------

void UARPGFluidSurfaceSubsystem::StepSimulation(float DeltaTime)
{
	TickWeather(DeltaTime);
}

void UARPGFluidSurfaceSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!HasAuthority())
	{
		return;
	}

	const float Interval = 1.f / FMath::Max(0.5f, TickRate);

	TickAccumulator += DeltaTime;
	if (TickAccumulator < Interval)
	{
		return;
	}

	StepSimulation(TickAccumulator);
	TickAccumulator = 0.f;
}

void UARPGFluidSurfaceSubsystem::TickWeather(float DeltaTime)
{
	for (int32 Index = Pools.Num() - 1; Index >= 0; --Index)
	{
		AARPGFluidPool* Pool = Pools[Index];
		if (!IsValid(Pool) || !Pool->Definition)
		{
			Pools.RemoveAt(Index);
			continue;
		}

		// Rain grows, sun shrinks. Both are the SAME operation with opposite
		// sign, which is the entire reason a body is a polygon.
		const float Rate = bRaining
			? Pool->Definition->RainGrowthRate
			: -Pool->Definition->EvaporationRate;

		if (FMath::IsNearlyZero(Rate))
		{
			continue;
		}

		const TArray<FVector2D> Next = ARPGFluidGeometry::OffsetRing(Pool->GetRing(), Rate * DeltaTime);

		// Below the floor, or eroded away entirely. The floor exists because an
		// evaporating pool's area approaches zero asymptotically -- without it a
		// sliver would live forever, costing a rebuild every tick to become
		// imperceptibly smaller.
		if (ARPGFluidGeometry::PolygonArea(Next) < Pool->Definition->MinimumArea)
		{
			Pools.RemoveAt(Index);
			Pool->Destroy();
			continue;
		}

		Pool->SetRing(Next);
	}

	for (int32 Index = ActiveSolids.Num() - 1; Index >= 0; --Index)
	{
		AARPGFluidSolid* Solid = ActiveSolids[Index];
		if (!IsValid(Solid) || !Solid->Definition)
		{
			ActiveSolids.RemoveAt(Index);
			continue;
		}

		if (FMath::IsNearlyZero(Solid->Definition->MeltRate))
		{
			continue; // permanent -- obsidian is rock, not frozen lava
		}

		const TArray<FVector2D> Next = ARPGFluidGeometry::OffsetRing(
			Solid->GetRing(), -Solid->Definition->MeltRate * DeltaTime);

		if (ARPGFluidGeometry::PolygonArea(Next) < Solid->Definition->MinimumArea)
		{
			// Melting returns its area to the fluid it came from, rather than
			// the water simply vanishing when a floe goes.
			if (Solid->Definition->MeltsInto)
			{
				const FVector2D Centre = ARPGFluidGeometry::PolygonCentroid(Solid->GetRing());
				const double Radius = FMath::Sqrt(
					FMath::Max(1.0, ARPGFluidGeometry::PolygonArea(Solid->GetRing())) / PI);

				DepositRing(ARPGFluidGeometry::MakeCircle(Centre, Radius),
					Solid->GroundHeight, Solid->Definition->MeltsInto);
			}

			ActiveSolids.RemoveAt(Index);
			Solid->Destroy();
			continue;
		}

		Solid->SetRing(Next);

		// The outer edge shrinks while the hole WIDENS -- both are erosion, and
		// keeping the rings apart is what makes that fall out rather than
		// needing a special case.
		if (Solid->HoleRing.Num() >= 3)
		{
			Solid->HoleRing = ARPGFluidGeometry::OffsetRing(
				Solid->HoleRing, Solid->Definition->MeltRate * DeltaTime);
		}
	}
}
