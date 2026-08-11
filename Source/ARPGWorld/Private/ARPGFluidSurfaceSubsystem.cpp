// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGFluidSurfaceSubsystem.h"
#include "ARPGDischargeContext.h"
#include "ARPGDischargeEffect.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGFluidBody.h"
#include "ARPGFluidDefinition.h"
#include "ARPGFluidGeometry.h"
#include "ARPGFreezableSurface.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicElement.h"
#include "ARPGWorld.h"
#include "ARPGWorldSettings.h"
#include "Engine/World.h"

namespace
{
	/**
	 * How far ABOVE a finished spell the ground probe starts.
	 *
	 * A spell that ended a little inside the floor -- a projectile destroyed one
	 * frame after it began penetrating -- traces from inside the geometry, and a
	 * line trace starting inside a body does not report its surface. Lifting the
	 * start clear costs nothing and is the difference between that spell wetting
	 * the ground and wetting nothing.
	 */
	constexpr float GroundProbeLift = 100.f;

	/**
	 * The freezable surface behind a volume.
	 *
	 * Two places to look, and both are legitimate. A RESERVOIR volume IS the
	 * surface -- a river is a component bolted onto whatever actor the level
	 * happens to use, and there is no ARPG actor class to reach for. A fluid POOL
	 * owns its volume, so the surface is the actor above it. One rule, asked in
	 * one place, rather than every caller knowing which shape it has.
	 */
	IARPGFreezableSurface* FindFreezable(UARPGElementalVolumeComponent* Volume)
	{
		if (!Volume)
		{
			return nullptr;
		}

		if (IARPGFreezableSurface* Direct = Cast<IARPGFreezableSurface>(Volume))
		{
			return Direct;
		}

		return Cast<IARPGFreezableSurface>(Volume->GetOwner());
	}
}

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

	if (Solids.Num() == 0)
	{
		for (const TSoftObjectPtr<UARPGSolidDefinition>& Soft : Settings.SolidDefinitions)
		{
			if (UARPGSolidDefinition* Definition = Soft.LoadSynchronous())
			{
				Solids.Add(Definition);
			}
			else if (!Soft.IsNull())
			{
				UE_LOG(LogARPGWorld, Warning,
					TEXT("Solid definition '%s' from project settings failed to load."),
					*Soft.ToString());
			}
		}
	}

	if (!CombinationTable)
	{
		CombinationTable = Settings.CombinationTable.LoadSynchronous();
	}

	DischargeLandedHandle = AARPGDischargeEffect::OnDischargeLanded.AddUObject(
		this, &UARPGFluidSurfaceSubsystem::HandleDischargeLanded);
}

void UARPGFluidSurfaceSubsystem::Deinitialize()
{
	AARPGDischargeEffect::OnDischargeLanded.Remove(DischargeLandedHandle);
	Super::Deinitialize();
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

	// LOUDLY, and once. An element with no fluid definition is ordinary -- fire
	// does not pool, and that is why Deposit's own miss is only Verbose. NOTHING
	// having one is a configuration mistake with no other symptom whatsoever:
	// spells cast, spells land, and the ground silently stays dry, which is
	// exactly the state this project was in until its fluid settings were filled.
	//
	// Raised on the first deposit ASKED FOR rather than at startup, so a world
	// that never wanted a puddle -- and every test fixture that brings its own
	// definitions -- stays quiet.
	if (Definitions.Num() == 0 && !bWarnedNoDefinitions)
	{
		bWarnedNoDefinitions = true;

		UE_LOG(LogARPGWorld, Warning,
			TEXT("Something tried to deposit '%s' and NOTHING is configured to pool: no spell "
			     "will ever leave a body on the ground and nothing can be frozen. Fill in "
			     "Project Settings > Game > ARPG World > Fluids."),
			*ElementTag.ToString());
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
		// VERBOSE, not a warning. "This element does not pool" is the ordinary
		// answer for most of them -- fire, air, lightning -- and it is the answer
		// that keeps the branch out of the caller. The misconfiguration worth
		// shouting about is having NO definitions at all, which FindDefinition
		// covers.
		UE_LOG(LogARPGWorld, Verbose, TEXT("Nothing pools '%s'; deposited nothing."),
			*ElementTag.ToString());
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
		UE_LOG(LogARPGWorld, Verbose, TEXT("Nothing pools '%s'; deposited nothing."),
			*ElementTag.ToString());
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

	// TOO LITTLE TO BE A BODY. Nothing merged it, so this would be a new pool
	// already under the floor at which weather destroys it -- spawned, replicated
	// and gone within a tick.
	//
	// Checked HERE and not in the merge above, because being too small to be a
	// puddle of your own does not stop you adding to one: a light cast into
	// standing water still enlarges it, and the last of a melting floe still
	// returns its water to the pool it froze out of. What is refused is only the
	// body that would have nothing to belong to.
	if (ARPGFluidGeometry::PolygonArea(Footprint) < Definition->MinimumArea)
	{
		return nullptr;
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

bool UARPGFluidSurfaceSubsystem::TraceToGround(FVector From, const AActor* Ignore,
	FVector& OutGround) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	const FVector Start = From + FVector(0.f, 0.f, GroundProbeLift);
	const FVector End = From - FVector(0.f, 0.f, MaxDepositDrop);

	// VISIBILITY rather than WorldStatic, because it is the channel a designer can
	// opt a mesh OUT of. Dressing that a puddle should form under rather than on
	// top of -- grass, debris, a fallen banner -- already ignores it.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ARPGFluidDeposit), /*bTraceComplex=*/false);

	// The spell is still in the world at the moment it finishes, so without this a
	// projectile with a collider lands on ITSELF and the puddle forms in mid-air.
	Params.AddIgnoredActor(Ignore);

	FHitResult Hit;
	if (!World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
	{
		return false;
	}

	OutGround = Hit.ImpactPoint;
	return true;
}

void UARPGFluidSurfaceSubsystem::HandleDischargeLanded(AARPGDischargeEffect* Effect)
{
	// OnDischargeLanded is process-wide, so a PIE session running a server and a
	// client world would otherwise each deposit the other's puddles. Same rule as
	// OnVolumesMet, and for the same reason.
	if (!Effect || Effect->GetWorld() != GetWorld())
	{
		return;
	}

	// Pools are replicated actors the server owns -- see HasAuthority. The effect
	// replicates down, so this fires on clients too, and a client depositing its
	// own copy would leave every puddle in the level doubled.
	if (!HasAuthority())
	{
		return;
	}

	const FARPGDischargeContext& Context = Effect->GetDischargeContext();
	if (!Context.PrimaryElement || Effect->DepositRadius <= 0.f)
	{
		return;
	}

	const FGameplayTag ElementTag = Context.PrimaryElement->ElementTag;

	// Asked BEFORE tracing, so the overwhelmingly common case -- a fire spell,
	// which pools nothing -- costs a lookup rather than a line trace.
	if (!FindDefinition(ElementTag))
	{
		return;
	}

	FVector Landed;
	if (!TraceToGround(Effect->GetActorLocation(), Effect, Landed))
	{
		// Expired over a drop or too high above the floor. It wet nothing, which
		// is an outcome and not a failure.
		return;
	}

	// A jet's footprint runs from where it was cast to where it ended; a
	// projectile's is a disc where it landed. See bDepositSwept. A jet cast from
	// somewhere with no ground beneath it -- off a ledge, over a stairwell -- falls
	// back to the disc rather than depositing nothing, since the far end plainly
	// finished somewhere real.
	FVector CastFrom;
	if (Effect->bDepositSwept && TraceToGround(Context.Origin, Effect, CastFrom))
	{
		DepositSwept(CastFrom, Landed, Effect->DepositRadius, ElementTag);
		return;
	}

	Deposit(Landed, Effect->DepositRadius, ElementTag);
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

	// One side has to be a SURFACE -- something with an extent lying there to be
	// frozen. That used to mean "is literally an AARPGFluidPool", which quietly
	// restricted freezing to bodies this subsystem had spawned: an authored river,
	// the case the reservoir idea exists for, failed the cast and fell through to
	// an ordinary energy trade. See IARPGFreezableSurface.
	IARPGFreezableSurface* Surface = FindFreezable(A);
	UARPGElementalVolumeComponent* Agent = B;
	UARPGElementalVolumeComponent* Frozen = A;

	if (!Surface)
	{
		Surface = FindFreezable(B);
		Agent = A;
		Frozen = B;
	}

	if (!Surface || !Agent->OverlapSource)
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
	const FVector2D AgentCentre(AgentBounds.Origin.X, AgentBounds.Origin.Y);
	const double AgentRadius = FMath::Max(AgentBounds.BoxExtent.X, AgentBounds.BoxExtent.Y);

	const TArray<FVector2D> AgentRing = ARPGFluidGeometry::MakeCircle(AgentCentre, AgentRadius);

	// Bounded by the contact rather than asked for whole, because a river is
	// kilometres long and only the metre the shard touched is a candidate. A pool
	// ignores the bound and hands back its ring.
	const TArray<FVector2D> Footprint = Surface->GetFreezableFootprint(AgentCentre, AgentRadius);

	// THE OVERLAP, not the whole surface and not the whole shard. Freezing exactly
	// where the two met is the entire reason a body is a polygon rather than a
	// disc or a grid cell.
	TArray<FVector2D> FrozenRing;
	TArray<TArray<FVector2D>> Holes;
	ARPGFluidGeometry::IntersectWithHoles(Footprint, AgentRing, FrozenRing, Holes);

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

	const float SurfaceHeight = Surface->GetFreezableSurfaceHeight(Centre);

	AARPGFluidSolid* Solid = World->SpawnActor<AARPGFluidSolid>(
		AARPGFluidSolid::StaticClass(),
		FTransform(FVector(Centre.X, Centre.Y, SurfaceHeight)), Params);

	if (!Solid)
	{
		return false;
	}

	// Only the largest hole is carried: a solid keeps its gaps, but tracking an
	// arbitrary set of them buys nothing a single dominant gap does not, and
	// each extra ring is another thing erosion has to keep clean.
	//
	// SET BEFORE Setup, which is what builds the slab's mesh and its collision.
	// Assigning it afterwards -- which is what this did while a hole only affected
	// IsStandableAt -- now leaves a floe drawn and walkable straight over its own
	// gap until something else happens to rebuild it.
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

	Solid->Setup(SolidDefinition, FrozenRing, SurfaceHeight);

	ActiveSolids.Add(Solid);

	// The fluid is genuinely used up -- unless it is bottomless, which is the
	// surface's own answer to give. A puddle shrinks and may be finished by this;
	// a river takes nothing and is never finished.
	if (Surface->ConsumeFreezableArea(FrozenArea))
	{
		// Only this subsystem's own bodies are its to retire. Anything else that
		// reports itself used up owns its own lifetime.
		if (AARPGFluidPool* Spent = Cast<AARPGFluidPool>(Surface->_getUObject()))
		{
			Pools.Remove(Spent);
			Spent->Destroy();
		}
	}

	// Spent freezing it.
	Agent->Consume(Agent->GetEnergy(), Entry->Result);

	UE_LOG(LogARPGWorld, Log, TEXT("Froze %.0f square units of '%s' into '%s'."),
		FrozenArea, *Frozen->Element->ElementTag.ToString(),
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

		// The outer edge shrinks while the hole WIDENS -- both are erosion, and
		// keeping the rings apart is what makes that fall out rather than
		// needing a special case.
		//
		// Widened BEFORE the ring is set, because setting the ring is what rebuilds
		// the mesh and the collision, and both read the hole. The other order draws
		// this tick's outline around last tick's gap.
		if (Solid->HoleRing.Num() >= 3)
		{
			Solid->HoleRing = ARPGFluidGeometry::OffsetRing(
				Solid->HoleRing, Solid->Definition->MeltRate * DeltaTime);
		}

		Solid->SetRing(Next);
	}
}
