// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGFluidSurfaceSubsystem.h"
#include "ARPGDischargeContext.h"
#include "ARPGDischargeEffect.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGSurfaceBody.h"
#include "ARPGSolidBody.h"
#include "ARPGFluidDefinition.h"
#include "ARPGSolidDefinition.h"
#include "ARPGFluidGeometry.h"
#include "ARPGElementalSurface.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicElement.h"
#include "ARPGWorld.h"
#include "ARPGWorldSettings.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "SignificanceManager.h"

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
	IARPGElementalSurface* FindSurface(UARPGElementalVolumeComponent* Volume)
	{
		if (!Volume)
		{
			return nullptr;
		}

		if (IARPGElementalSurface* Direct = Cast<IARPGElementalSurface>(Volume))
		{
			return Direct;
		}

		return Cast<IARPGElementalSurface>(Volume->GetOwner());
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

AARPGFluidPool* UARPGFluidSurfaceSubsystem::ReturnFluid(FVector2D Where, float GroundHeight,
	double Volume, UARPGFluidDefinition* Definition)
{
	if (Volume <= 0.0 || !Definition)
	{
		return nullptr;
	}

	// INTO A BODY THAT IS ALREADY THERE, by growing it rather than by depositing a
	// disc on top of it.
	//
	// THIS IS THE CASE THAT LOOKS FINE AND LOSES THE WATER. Fluid returns where it
	// left, so the disc almost always lands INSIDE the pool it is joining -- and a
	// disc unioned with an outline that already contains it is that same outline.
	// Merging would have conserved nothing, silently, in the common case.
	for (AARPGFluidPool* Pool : Pools)
	{
		if (IsValid(Pool) && Pool->Definition == Definition
			&& Pool->ContainsPoint(FVector(Where.X, Where.Y, GroundHeight)))
		{
			Pool->AbsorbSurfaceVolume(Volume);
			return Pool;
		}
	}

	// Nowhere to put it but the ground. VOLUME BECOMES AREA THROUGH DEPTH, and
	// this is the only place in the system that does it: a puddle has no third
	// dimension of its own -- pour more in and it gets wider, not deeper -- so the
	// fluid's Depth is exactly the exchange rate between how much of it there is
	// and how much ground it covers.
	const double Area = Volume / FMath::Max(1.f, Definition->Depth);
	const double Radius = FMath::Sqrt(Area / PI);

	return DepositRing(ARPGFluidGeometry::MakeCircle(Where, Radius), GroundHeight, Definition);
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

	// AND EVERY BODY THIS SYSTEM OWNS, because a body is not ground.
	//
	// A slab blocks every channel -- it has to, you stand on it -- so a spell that
	// finished over a floe traced down, hit the ICE, and left its puddle on top:
	// at the wrong height, and as a separate actor that does not drift with the
	// floe, so it stayed hanging over the water once the floe moved on. What that
	// spell wet is whatever the floe is floating in, which is what the trace finds
	// with the floe out of its way. A pool is skipped for the milder version of
	// the same reason -- landing on the surface film of a puddle rather than the
	// bed under it would stack a second body a couple of centimetres above the
	// first instead of merging with it.
	for (const AARPGFluidPool* Pool : Pools)
	{
		if (IsValid(Pool))
		{
			Params.AddIgnoredActor(Pool);
		}
	}

	for (const AARPGSolidBody* Solid : ActiveSolids)
	{
		if (IsValid(Solid))
		{
			Params.AddIgnoredActor(Solid);
		}
	}

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
	// an ordinary energy trade. See IARPGElementalSurface.
	IARPGElementalSurface* Surface = FindSurface(A);
	UARPGElementalVolumeComponent* Agent = B;
	UARPGElementalVolumeComponent* Frozen = A;

	if (!Surface)
	{
		Surface = FindSurface(B);
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
	const TArray<FVector2D> Footprint = Surface->GetSurfaceFootprint(AgentCentre, AgentRadius);

	// THE OVERLAP, not the whole surface and not the whole shard. Freezing exactly
	// where the two met is the entire reason a body is a polygon rather than a
	// disc or a grid cell.
	TArray<FVector2D> SolidifiedRing;
	TArray<TArray<FVector2D>> Holes;
	ARPGFluidGeometry::IntersectWithHoles(Footprint, AgentRing, SolidifiedRing, Holes);

	const double SolidifiedArea = ARPGFluidGeometry::PolygonArea(SolidifiedRing);
	if (SolidifiedArea < SolidDefinition->MinimumArea)
	{
		return false;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	const FVector2D Centre = ARPGFluidGeometry::PolygonCentroid(SolidifiedRing);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	const float SurfaceHeight = Surface->GetSurfaceLevelAt(Centre);

	AARPGSolidBody* Solid = World->SpawnActor<AARPGSolidBody>(
		AARPGSolidBody::StaticClass(),
		FTransform(FVector(Centre.X, Centre.Y, SurfaceHeight)), Params);

	if (!Solid)
	{
		return false;
	}

	// WHAT IT RIDES. A floe is not an independent object: it asks this for the
	// waterline under it, for the current carrying it, and for where the water
	// stops. Set before Setup, which is what first places it.
	Solid->FloatsOn = Cast<UObject>(Surface);

	Solid->Setup(SolidDefinition, SolidifiedRing, SurfaceHeight);

	// Immediately, so a plug frozen across a river is anchored on the frame it
	// forms rather than drifting for a quarter of a second first.
	Solid->UpdateAnchoring();

	ActiveSolids.Add(Solid);

	// The fluid is genuinely used up -- unless it is bottomless, which is the
	// surface's own answer to give. A puddle shrinks and may be finished by this;
	// a river takes nothing and is never finished.
	if (Surface->ConsumeSurfaceArea(SolidifiedArea))
	{
		// Only this subsystem's own bodies are its to retire. Anything else that
		// reports itself used up owns its own lifetime.
		RetireBody(Cast<AARPGSurfaceBody>(Surface));
	}

	// Spent freezing it.
	Agent->Consume(Agent->GetEnergy(), Entry->Result);

	UE_LOG(LogARPGWorld, Log, TEXT("Froze %.0f square units of '%s' into '%s'."),
		SolidifiedArea, *Frozen->Element->ElementTag.ToString(),
		*SolidDefinition->GetElementTag().ToString());

	return true;
}

void UARPGFluidSurfaceSubsystem::NoteReactionContact(UARPGElementalVolumeComponent* A,
	UARPGElementalVolumeComponent* B, FVector Contact)
{
	const FVector2D Where(Contact.X, Contact.Y);

	for (UARPGElementalVolumeComponent* Side : { A, B })
	{
		if (AARPGSolidBody* Slab = Side ? Cast<AARPGSolidBody>(Side->GetOwner()) : nullptr)
		{
			Slab->NoteContactAt(Where);
		}
	}
}

void UARPGFluidSurfaceSubsystem::OpenReactionLedger()
{
	FluidReturnedThisReaction.Reset();
}

void UARPGFluidSurfaceSubsystem::NoteFluidReturned(FGameplayTag ElementTag)
{
	// Whether it became a puddle or vanished into a lake. Both are the material
	// being accounted for; only one of them is visible, and neither leaves room
	// for the product to deposit the same fluid again.
	FluidReturnedThisReaction.AddUnique(ElementTag);
}

bool UARPGFluidSurfaceSubsystem::WasFluidReturned(FGameplayTag ElementTag) const
{
	return FluidReturnedThisReaction.Contains(ElementTag);
}

void UARPGFluidSurfaceSubsystem::DropRiders(AARPGFluidPool* Pool)
{
	// ANYTHING FLOATING ON IT GOES TOO. Nothing linked a floe's life to the water
	// under it, so boiling a pool out from beneath one -- which fire can now do --
	// left ice hanging in the air over dry ground. A floe is not an independent
	// object; it is a thing riding a surface, and there is no surface left to
	// ride. True however the pool went, which is why this is not inlined into the
	// one path that used to be the only way for a pool to go.
	for (int32 Index = ActiveSolids.Num() - 1; Index >= 0; --Index)
	{
		AARPGSolidBody* Riding = ActiveSolids[Index];
		if (IsValid(Riding) && Riding->FloatsOn.GetObject() == Pool)
		{
			ActiveSolids.RemoveAt(Index);
			Riding->Destroy();
		}
	}
}

void UARPGFluidSurfaceSubsystem::RetireBody(AARPGSurfaceBody* Body)
{
	if (!IsValid(Body))
	{
		return;
	}

	if (AARPGFluidPool* Pool = Cast<AARPGFluidPool>(Body))
	{
		Pools.Remove(Pool);
		DropRiders(Pool);
		Pool->Destroy();
		return;
	}

	AARPGSolidBody* Solid = Cast<AARPGSolidBody>(Body);
	if (!Solid)
	{
		return;
	}

	// A SOLID RETURNS NOTHING WHEN IT GOES, and this used to be where it did.
	//
	// The two ways a slab reaches nothing are not the same event. A reaction melts
	// it, and the water for that is deposited by the reaction, at the point of
	// contact, as it happens -- see ReturnMeltedFluid. Ambient melting is the other,
	// and it is meant to return nothing at all: a floe thinning in the sun over a
	// minute should leave dry ground, not a puddle appearing out of nowhere at the
	// instant the last of it goes.
	//
	// Depositing here could not tell those apart, and got both wrong: it fired for
	// the ambient case that wanted nothing, and for the reaction case it offered a
	// second helping sized by whatever sliver was left -- which, being under
	// MinimumArea by definition, was refused anyway.
	ActiveSolids.Remove(Solid);
	Solid->Destroy();
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

float UARPGFluidSurfaceSubsystem::GroundUnder(FVector WorldPosition, const AActor* Ignore) const
{
	FVector Ground;
	return TraceToGround(WorldPosition, Ignore, Ground)
		? static_cast<float>(Ground.Z)
		: static_cast<float>(WorldPosition.Z);
}

AARPGSolidBody* UARPGFluidSurfaceSubsystem::FindSolidAt(FVector WorldPosition) const
{
	for (AARPGSolidBody* Solid : ActiveSolids)
	{
		if (IsValid(Solid) && Solid->ContainsPoint(WorldPosition))
		{
			return Solid;
		}
	}
	return nullptr;
}

AARPGSolidBody* UARPGFluidSurfaceSubsystem::FindSolidNear(FVector WorldPosition, float Reach,
	FGameplayTag ElementTag) const
{
	AARPGSolidBody* Nearest = nullptr;
	double Closest = static_cast<double>(Reach) * Reach;

	for (AARPGSolidBody* Solid : ActiveSolids)
	{
		if (!IsValid(Solid))
		{
			continue;
		}

		// An empty tag matches anything, which is the honest default for "is there
		// a slab here" -- filtering is the caller's business and most callers have
		// exactly one element in mind.
		if (ElementTag.IsValid() && Solid->Volume
			&& Solid->Volume->Element
			&& Solid->Volume->Element->ElementTag != ElementTag)
		{
			continue;
		}

		// TO THE SLAB, not to its origin. A wall is long, and a caster standing at
		// one end of one is not far from it -- measuring to the actor would say
		// they were, because a slab's origin is its centroid.
		//
		// Asked of the BODY rather than the field, because the field is in its own
		// frame now and the body is the one thing that knows where that is.
		const double Gap = Solid->DistanceToEdge(FVector2D(WorldPosition.X, WorldPosition.Y));
		const double GapSq = Gap * Gap;

		if (GapSq <= Closest)
		{
			Closest = GapSq;
			Nearest = Solid;
		}
	}

	return Nearest;
}

void UARPGFluidSurfaceSubsystem::RegisterSolid(AARPGSolidBody* Solid)
{
	if (IsValid(Solid))
	{
		ActiveSolids.AddUnique(Solid);
	}
}

void UARPGFluidSurfaceSubsystem::UnregisterSolid(AARPGSolidBody* Solid)
{
	ActiveSolids.Remove(Solid);
}

bool UARPGFluidSurfaceSubsystem::IsCoveredBySolid(FVector WorldPosition) const
{
	for (const AARPGSolidBody* Solid : ActiveSolids)
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

void UARPGFluidSurfaceSubsystem::GatherViewers()
{
	ViewerLocations.Reset();

	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// THE ENGINE'S LIST FIRST. USignificanceManager already holds the viewpoints
	// and is updated where the engine knows they have moved, which is both more
	// correct than sampling them on a 4Hz weather tick and shared with anything
	// else that asks the same question.
	if (const USignificanceManager* Significance = USignificanceManager::Get(World))
	{
		for (const FTransform& Viewpoint : Significance->GetViewpoints())
		{
			ViewerLocations.Add(Viewpoint.GetLocation());
		}

		if (ViewerLocations.Num() > 0)
		{
			return;
		}
	}

	// AND THE PLAYER LIST WHEN THERE IS NO MANAGER, which is not a dead branch:
	// one exists only where a game mode has spawned it, and no automation fixture
	// does. A dedicated server with no local viewpoint lands here too and finds
	// nothing, which IsSignificantAt reads as "everything matters".
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* Controller = It->Get();
		if (!Controller)
		{
			continue;
		}

		// The PAWN where there is one, so a body near the player counts even while
		// the camera is somewhere else -- a floe you are standing on has to keep
		// its collision whatever you are looking at.
		if (const APawn* Pawn = Controller->GetPawn())
		{
			ViewerLocations.Add(Pawn->GetActorLocation());
		}
		else if (const AActor* ViewTarget = Controller->GetViewTarget())
		{
			ViewerLocations.Add(ViewTarget->GetActorLocation());
		}
	}
}

bool UARPGFluidSurfaceSubsystem::IsSignificantAt(FVector WorldPosition) const
{
	// NOBODY WATCHING MEANS EVERYTHING MATTERS. A dedicated server with no local
	// viewer, and every automation fixture, would otherwise quietly switch the
	// whole system off -- which is the kind of optimisation that only shows up as
	// a test that passes for the wrong reason.
	if (ViewerLocations.Num() == 0)
	{
		return true;
	}

	const float ReachSq = SignificanceDistance * SignificanceDistance;

	for (const FVector& Viewer : ViewerLocations)
	{
		if (FVector::DistSquared(Viewer, WorldPosition) <= ReachSq)
		{
			return true;
		}
	}

	return false;
}

void UARPGFluidSurfaceSubsystem::EnforceBudget()
{
	// THE SMALLEST GOES. It is the cheapest thing to lose and the least likely to
	// be the one the player is standing in -- and losing something is the point:
	// nothing else in the system bounds how many bodies a session accumulates.
	//
	// NOT VIA RetireBody, deliberately. Retiring is a body reaching its natural
	// end, and a solid that reaches its end puts its water back -- which under a
	// budget cull would answer "too many bodies" by making another one. This is
	// the world giving up on something, so it simply goes.
	auto Trim = [this](auto& Register, const TCHAR* Kind)
	{
		while (Register.Num() > MaxBodiesOfEachKind)
		{
			int32 Smallest = INDEX_NONE;
			double LeastWorth = TNumericLimits<double>::Max();

			for (int32 Index = 0; Index < Register.Num(); ++Index)
			{
				if (!IsValid(Register[Index]))
				{
					Smallest = Index;
					break;
				}

				// PERMANENT BODIES ARE NOT SPARE CAPACITY. The budget exists for
				// litter -- puddles a player left behind crossing a field, floes
				// that will melt anyway -- and culling those is unnoticeable. An
				// earth wall someone raised for cover is the opposite: it is there
				// on purpose, nothing was ever going to remove it, and having it
				// vanish mid-fight because the level accumulated puddles elsewhere
				// is the worst outcome this economy could produce.
				if (Register[Index]->IsPermanent())
				{
					continue;
				}

				// SIGNIFICANCE FIRST, AREA AS THE TIE-BREAK. Area alone picked
				// the wrong body with some regularity: the puddle at the
				// player's feet is small and the field they crossed ten minutes
				// ago is large, so a pure area cull reliably dropped the one
				// being looked at. USignificanceManager already answers "how
				// much does this matter" against the engine's own viewpoints,
				// and multiplying keeps area meaningful among equals -- between
				// two puddles the same distance away, the smaller still goes.
				//
				// A zero-significance body scores zero whatever its area, which
				// is the intent: nothing is watching it.
				const double Worth = Register[Index]->GetSignificance() * Register[Index]->GetArea();
				if (Worth < LeastWorth)
				{
					LeastWorth = Worth;
					Smallest = Index;
				}
			}

			// Everything left is permanent, so the register stays over budget --
			// which is the right answer. A cap is a limit on what the world keeps
			// FOR you, not a licence to delete what you built.
			if (Smallest == INDEX_NONE)
			{
				UE_LOG(LogARPGWorld, Verbose,
					TEXT("Over the %s budget and everything left is permanent."), Kind);
				break;
			}

			UE_LOG(LogARPGWorld, Verbose,
				TEXT("Over the %s budget, so the least significant one goes."), Kind);

			AARPGSurfaceBody* Spent = Register[Smallest];
			Register.RemoveAt(Smallest);

			// Whatever was riding it goes with it, exactly as when a pool is
			// retired -- a culled pool leaving its floe over dry ground would be
			// the one visible artefact this whole economy could produce.
			if (AARPGFluidPool* Pool = Cast<AARPGFluidPool>(Spent))
			{
				DropRiders(Pool);
			}

			if (IsValid(Spent))
			{
				Spent->Destroy();
			}
		}
	};

	Trim(Pools, TEXT("pool"));
	Trim(ActiveSolids, TEXT("solid"));
}

void UARPGFluidSurfaceSubsystem::StepSimulation(float DeltaTime)
{
	GatherViewers();
	TickWeather(DeltaTime);
	EnforceBudget();
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
		AARPGSolidBody* Solid = ActiveSolids[Index];
		if (!IsValid(Solid) || !Solid->Definition)
		{
			ActiveSolids.RemoveAt(Index);
			continue;
		}

		if (FMath::IsNearlyZero(Solid->Definition->MeltRate))
		{
			continue; // permanent -- obsidian is rock, not frozen lava
		}

		// AMBIENT MELT IS A THINNING, not an inward offset of an outline. Both
		// faces at once, because a floe in water melts from underneath as much as
		// from above -- and every hole in it widens for free, since a hole is just
		// the cells where the two faces have already met. No ring to offset, no
		// slit to round into arcs, nothing that can grow.
		const float Thinning = Solid->Definition->MeltRate * DeltaTime;
		Solid->MeltUniformly(Thinning * 0.5f, Thinning * 0.5f);

		if (Solid->GetArea() < Solid->Definition->MinimumArea)
		{
			RetireBody(Solid);
			continue;
		}

		// A floe that has narrowed enough to come free of the banks COMES FREE.
		// Here rather than per frame: this is the tick that changes its shape, and
		// it costs a containment probe per direction.
		Solid->UpdateAnchoring();
	}
}
