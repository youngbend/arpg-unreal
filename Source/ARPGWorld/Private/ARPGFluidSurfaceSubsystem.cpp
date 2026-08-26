// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGFluidSurfaceSubsystem.h"
#include "ARPGDischargeContext.h"
#include "ARPGDischargeEffect.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGFluidRegion.h"
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
#include "EngineUtils.h"
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

void UARPGFluidSurfaceSubsystem::PourIntoField(const FVector2D& Where, float NearZ,
	double Volume, float MaxRadius, UARPGFluidDefinition* Definition)
{
	if (FARPGFluidField* Field = FindField(Definition))
	{
		Field->PourVolume(Where, NearZ, Volume, MaxRadius);
	}
}

AARPGFluidRegion* UARPGFluidSurfaceSubsystem::Deposit(FVector WorldPosition, float Radius,
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

	const FVector2D At(WorldPosition.X, WorldPosition.Y);

	// NO CLIPPING, AND NO MERGE PASS. Both were the polygon model working around
	// having one flat outline per body: ClipToGround pulled a disc back along its
	// own spokes until every vertex was on ground within a step of the middle, and
	// DepositRing then hunted for pools near enough to union with. The field needs
	// neither. It never lays fluid on ground it has not sampled, it will not cross
	// into a cell with no floor under it, and two pours that touch are one body
	// because their cells touch.
	//
	// WHICH IS ALSO WHY A LEDGE STILL STOPS IT. The head between a cell and one
	// whose bed is far below is a fall rather than a slope, and the fall is capped
	// at the fluid's WallSpeed -- so water creeps off a lip rather than teleporting
	// down it, and refuses entirely where there is no floor at all.
	const double Volume = static_cast<double>(PI) * Radius * Radius * Definition->Depth;

	PourIntoField(At, WorldPosition.Z, Volume, Radius, Definition);

	return SettleRegionAt(At, WorldPosition.Z, Definition);
}

AARPGFluidRegion* UARPGFluidSurfaceSubsystem::ReturnFluid(FVector2D Where, float GroundHeight,
	double Volume, UARPGFluidDefinition* Definition)
{
	if (Volume <= 0.0 || !Definition)
	{
		return nullptr;
	}

	// A MELT IS THE CASE THE FIELD EXISTS FOR, and the one this whole rewrite was
	// asked for. A slab hands back the material that melted, at the point it
	// melted, and it runs from there -- around whatever is standing in the way.
	// The polygon model could only grow the whole outline uniformly, somewhere the
	// heat never reached.
	//
	// AND THERE IS NO LONGER A SECOND PATH. ReturnFluid used to look for a pool
	// that already contained the point and grow it, precisely because a disc
	// unioned into an outline that already contained it vanished without trace.
	// Pouring into cells cannot lose water: it goes where it is put.
	const double Area = Volume / FMath::Max(1.f, Definition->Depth);

	PourIntoField(Where, GroundHeight, Volume,
		FMath::Max(static_cast<float>(FMath::Sqrt(Area / PI)), 1.f), Definition);

	return SettleRegionAt(Where, GroundHeight, Definition);
}

AARPGFluidRegion* UARPGFluidSurfaceSubsystem::DepositSwept(FVector From, FVector To, float Radius,
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
	const FVector2D Start(From.X, From.Y);
	const FVector2D Finish(To.X, To.Y);
	const double Length = FVector2D::Distance(Start, Finish);

	const TArray<FVector2D> Footprint = ARPGFluidGeometry::MakeStadium(Start, Finish, Radius);
	const double Volume = ARPGFluidGeometry::PolygonArea(Footprint) * Definition->Depth;

	// POURED ALONG THE SWEEP rather than as one disc over the middle of it. A
	// stadium two metres long is not a circle, and laying it as one would put a
	// dragged spell's water in a puddle at the halfway point.
	const int32 Pours = FMath::Clamp(FMath::CeilToInt(Length / FMath::Max(1.f, Radius)), 1, 16);
	const float NearZ = FMath::Min(From.Z, To.Z);

	for (int32 Step = 0; Step < Pours; ++Step)
	{
		const double Alpha = (Pours == 1) ? 0.5 : (Step / static_cast<double>(Pours - 1));
		PourIntoField(FMath::Lerp(Start, Finish, Alpha), NearZ, Volume / Pours, Radius, Definition);
	}

	return SettleRegionAt(FMath::Lerp(Start, Finish, 0.5), NearZ, Definition);
}

AARPGFluidRegion* UARPGFluidSurfaceSubsystem::SettleRegionAt(const FVector2D& At,
	float NearZ, UARPGFluidDefinition* Definition)
{
	// RECONCILED ON THE SPOT, rather than leaving the caller to wait a quarter of
	// a second for the next weather tick. A deposit returns the body the fluid
	// ended up in, and every caller and every test expects that to be true by the
	// time the call comes back.
	ReconcileRegions();

	// AND THE HEIGHT IS PART OF THE QUESTION. Two bodies can stand at the same XY
	// on different floors -- that is the whole point of layers -- so "which body
	// did this pour land in" cannot be answered in plan alone. The nearest floor
	// to where the caller was pouring is the one it poured onto.
	AARPGFluidRegion* Best = nullptr;
	float BestGap = TNumericLimits<float>::Max();

	for (AARPGFluidRegion* Region : Regions)
	{
		if (!IsValid(Region) || Region->Definition != Definition
			|| !Region->IsSurfaceAt(At))
		{
			continue;
		}

		const float Gap = FMath::Abs(Region->GetSurfaceBedAt(At) - NearZ);

		if (Gap < BestGap)
		{
			BestGap = Gap;
			Best = Region;
		}
	}

	return Best;
}

bool UARPGFluidSurfaceSubsystem::FindGroundAt(const FVector2D& At, float NearZ,
	float& OutHeight, const AActor* Ignore) const
{
	FVector Ground;
	if (!TraceToGround(FVector(At.X, At.Y, NearZ), Ignore, Ground))
	{
		return false;
	}

	OutHeight = static_cast<float>(Ground.Z);
	return true;
}

void UARPGFluidSurfaceSubsystem::ReconcileRegions()
{
	// PROXIES ARE MATCHED, NOT RESPAWNED. A floe whose surface was destroyed and
	// rebuilt every pass would be a floe that fell into the sea four times a
	// second -- AARPGSolidBody::FloatsOn points at one of these, and so does the
	// reaction solver's anti-cascade guard.
	TArray<AARPGFluidRegion*> Kept;
	TArray<AARPGFluidRegion*> Spare = Regions;

	Spare.RemoveAll([](const AARPGFluidRegion* Region) { return !IsValid(Region); });

	UWorld* World = GetWorld();

	if (!World)
	{
		return;
	}

	TArray<FARPGFluidField::FRegion> Bodies;

	for (TPair<TObjectPtr<UARPGFluidDefinition>, TUniquePtr<FARPGFluidField>>& Pair : Fields)
	{
		UARPGFluidDefinition* Definition = Pair.Key;
		FARPGFluidField& Field = *Pair.Value;

		if (!Definition)
		{
			continue;
		}

		Field.FindRegions(Bodies);

		for (const FARPGFluidField::FRegion& Body : Bodies)
		{
			// TOO LITTLE TO BE A BODY. The same floor a pool had, and for the same
			// reason: without one, a drying puddle would go on being an actor with
			// an overlap and a mesh long after there was anything to see.
			if (Body.Area < Definition->MinimumArea)
			{
				continue;
			}

			// MATCHED BY OVERLAP -- see AARPGFluidRegion::GetFingerprint. A proxy
			// that still has cells in this body IS this body, however much it has
			// grown, shrunk or changed shape since. Whichever proxy shares the most
			// with it wins, so when a puddle splits in two the larger half keeps
			// the actor and whatever was floating on it.
			int32 Match = INDEX_NONE;
			int32 BestShared = 0;

			for (int32 Which = 0; Which < Spare.Num(); ++Which)
			{
				AARPGFluidRegion* Candidate = Spare[Which];

				if (Candidate->Definition != Definition)
				{
					continue;
				}

				int32 Shared = 0;

				// WALKED OVER THE SMALLER OF THE TWO, because a body that has just
				// merged with its neighbour is far bigger than either proxy was.
				const TSet<TPair<FIntPoint, int32>>& Held = Candidate->GetCells();

				for (const TPair<FIntPoint, int32>& Cell : Held)
				{
					if (Body.Lookup.Contains(Cell))
					{
						++Shared;
					}
				}

				if (Shared > BestShared)
				{
					BestShared = Shared;
					Match = Which;
				}
			}

			AARPGFluidRegion* Region = nullptr;

			if (Match != INDEX_NONE)
			{
				Region = Spare[Match];
				Spare.RemoveAt(Match);
			}
			else
			{
				FActorSpawnParameters Params;
				Params.SpawnCollisionHandlingOverride =
					ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

				Region = World->SpawnActor<AARPGFluidRegion>(
					AARPGFluidRegion::StaticClass(),
					FTransform(FVector(Body.Centroid.X, Body.Centroid.Y, Body.MinBed)), Params);
			}

			if (!Region)
			{
				continue;
			}

			Region->UpdateFrom(&Field, Body, Definition);
			Kept.Add(Region);
		}
	}

	// WHAT NOTHING MATCHED IS GONE. A puddle that dried up, or one that split and
	// whose anchor ended up in the half that kept the proxy: either way there is no
	// water where this actor is, and anything floating on it has nothing to ride.
	Regions = MoveTemp(Kept);

	// WHAT NOTHING MATCHED IS GONE -- but its riders are re-homed before it goes,
	// not destroyed with it. A floe sitting on the boundary where two puddles
	// merged is still floating; it is floating on the body that swallowed the one
	// it knew about, and destroying it because its landlord changed name would be
	// the ice vanishing for a bookkeeping reason the player cannot see.
	for (AARPGFluidRegion* Orphan : Spare)
	{
		for (AARPGSolidBody* Riding : ActiveSolids)
		{
			if (!IsValid(Riding) || Riding->FloatsOn.GetObject() != Orphan)
			{
				continue;
			}

			const FVector2D At = Riding->GetWorldCentre();

			if (AARPGFluidRegion* Instead =
					FindRegionAt(FVector(At.X, At.Y, Riding->GetSurfaceHeight())))
			{
				Riding->FloatsOn = Instead;
			}
		}

		// Anything still pointing at it genuinely has nothing left to ride.
		DropRiders(Orphan);
		Orphan->Destroy();
	}
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

	// AND EVERY PAWN, because a person standing in a puddle is not the floor of
	// it. An emanation is cast AROUND its caster, so the caster is at dead centre
	// of the footprint every single time -- and a probe that stopped on their
	// capsule reported the floor as being wherever their shoulders were. The body
	// was then built to that height, its underside standing proud of the real
	// ground and shifting as they moved.
	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		Params.AddIgnoredActor(*It);
	}

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
	for (const AARPGFluidRegion* Region : Regions)
	{
		if (IsValid(Region))
		{
			Params.AddIgnoredActor(Region);
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

	// The field is server-authoritative -- see HasAuthority. The effect
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
	// frozen. That used to mean "is literally a pool actor", which quietly
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

	// NOT WHERE THERE IS ALREADY ICE. Freezing what is already frozen is not a
	// thing that can happen, and refusing it here is a hard floor under a whole
	// class of runaway: a slab is itself a body lying on the surface it froze
	// from, so anything that lets one act as an agent again freezes another patch,
	// and that one freezes another. On a BOTTOMLESS surface -- a river, which by
	// definition cannot be used up -- there is nothing else left to stop it, and
	// what that looked like was the game stopping inside a single frame while it
	// spawned slabs until it ran out of memory.
	//
	// The rider guard in UARPGElementalReactionSubsystem::Resolve is what should
	// catch this, and does. This is here because it was not enough on its own once
	// and the failure mode is a hang rather than a wrong answer.
	//
	// Cheap: it walks the live solids, and there are usually none.
	if (IsCoveredBySolid(FVector(Centre.X, Centre.Y, SurfaceHeight)))
	{
		return false;
	}

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

	// AND THE GAP IT FROZE AROUND. IntersectWithHoles has always handed these back
	// and this has always thrown them away -- a slab could not hold one, so
	// freezing over a pool with a gap in it produced solid ice. An outline can,
	// and the largest is the one worth keeping: a clip against a single round
	// agent produces at most one gap of any size.
	const TArray<FVector2D>* Largest = nullptr;
	double LargestArea = 0.0;

	for (const TArray<FVector2D>& Gap : Holes)
	{
		const double GapArea = ARPGFluidGeometry::PolygonArea(Gap);
		if (GapArea > LargestArea)
		{
			LargestArea = GapArea;
			Largest = &Gap;
		}
	}

	Solid->Setup(SolidDefinition, SolidifiedRing, SurfaceHeight,
		Largest ? *Largest : TArray<FVector2D>());

	// Immediately, so a plug frozen across a river is anchored on the frame it
	// forms rather than drifting for a quarter of a second first.
	Solid->UpdateAnchoring();

	ActiveSolids.Add(Solid);

	// THE REGION, not merely the amount. Ice occupies exactly the patch it froze
	// from, so the water has to lose that patch -- taking the area off uniformly
	// left a puddle the same shape as before with the new floe sitting on top of
	// water that had never receded. A river is bottomless and refuses either way.
	if (Surface->ConsumeSurfaceRegion(SolidifiedRing))
	{
		// THE ICE IS NOT A RIDER THAT LOST ITS WATER -- IT IS WHAT THE WATER
		// BECAME, and that distinction is the whole of this branch.
		//
		// Retiring a pool drops everything floating on it, which is exactly right
		// for the case DropRiders was written for: fire boils a puddle out from
		// under a floe and the floe has nothing left to ride. A freeze that takes
		// the LAST of a pool is the same call and the opposite event -- the slab
		// created a few lines above IS that pool, set solid and resting on the bed
		// it used to float in -- so the retire below would destroy the ice as part
		// of making it, and this function would go on to report success.
		//
		// THIS IS WHY FREEZING A PUDDLE LOOKED LIKE IT DID NOTHING. A partial
		// freeze leaves enough water to keep the pool alive, so the floe survives
		// and everything works; anything taking a pool under its minimum area
		// destroyed its own product and said it had frozen it. An emanation is
		// metres across and a puddle is not, so the common case in a real game was
		// the broken one -- and the only test on this path called TrySolidify with
		// a shard small enough to land on the other side of it.
		//
		// GROUNDING IT IS NOT A SPECIAL CASE. A null FloatsOn is already how this
		// codebase says "rooted" -- see AARPGRaiseSlabEffect, which makes rooted
		// slabs the same way, and the buoyancy tick, which reads it and leaves
		// them alone. A slab standing on the bed of a pool that is gone is rooted
		// by any reading.
		// ON THE BED, not at the waterline. Setup placed the slab at the surface it
		// froze out of, which is right for a floe because the buoyancy tick settles
		// it on the next frame -- and wrong for this one, because grounding it is
		// exactly what stops that tick from ever running. Left alone it hangs in
		// the air by the depth the puddle had.
		Solid->GroundOnBed(Surface->GetSurfaceBedAt(Centre));

		// Only this subsystem's own bodies are its to retire. Anything else that
		// reports itself used up owns its own lifetime.
		RetireBody(Cast<AARPGSurfaceBody>(Surface));
		RetireRegion(Cast<AARPGFluidRegion>(Cast<UObject>(Surface)));
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

void UARPGFluidSurfaceSubsystem::DropRiders(AActor* Body)
{
	// ANYTHING FLOATING ON IT GOES TOO. Nothing linked a floe's life to the water
	// under it, so boiling a pool out from beneath one left ice hanging in the air
	// over dry ground. A floe is not an independent object; it is a thing riding a
	// surface, and there is no surface left to ride.
	for (int32 Index = ActiveSolids.Num() - 1; Index >= 0; --Index)
	{
		AARPGSolidBody* Riding = ActiveSolids[Index];

		if (IsValid(Riding) && Riding->FloatsOn.GetObject() == Body)
		{
			ActiveSolids.RemoveAt(Index);
			Riding->Destroy();
		}
	}
}

void UARPGFluidSurfaceSubsystem::RetireRegion(AARPGFluidRegion* Region)
{
	if (!IsValid(Region))
	{
		return;
	}

	DropRiders(Region);
	Regions.Remove(Region);
	Region->Destroy();
}

void UARPGFluidSurfaceSubsystem::RetireBody(AARPGSurfaceBody* Body)
{
	if (!IsValid(Body))
	{
		return;
	}

	// NO FLUID BRANCH ANY MORE. A body of water is not something anything else
	// gets to retire: it exists exactly as long as there are connected wet cells
	// where it is, and ReconcileRegions is the only thing that decides that. What
	// a reaction does to water is take the water; the actor going is a consequence.

	AARPGSolidBody* Solid = Cast<AARPGSolidBody>(Body);
	if (!Solid)
	{
		return;
	}

	// A SOLID RETURNS NOTHING WHEN IT GOES, and this used to be where it did.
	//
	// The two ways a slab reaches nothing are not the same event. A reaction melts
	// it, and the water for that is deposited by the reaction, at the point of
	// contact, as it happens. Nothing else takes a slab at all now that ambient
	// melting is gone, so there is no second case for this to get wrong -- and a
	// second helping here would be sized by whatever sliver was left, which is
	// under MinimumArea by definition and would be refused anyway.
	ActiveSolids.Remove(Solid);
	Solid->Destroy();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

AARPGFluidRegion* UARPGFluidSurfaceSubsystem::FindRegionAt(FVector WorldPosition) const
{
	for (AARPGFluidRegion* Region : Regions)
	{
		if (IsValid(Region) && Region->ContainsPoint(WorldPosition))
		{
			return Region;
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

			// Whatever was riding it goes with it -- a culled body leaving its floe
			// over dry ground would be the one visible artefact this whole economy
			// could produce.
			DropRiders(Spent);

			if (IsValid(Spent))
			{
				Spent->Destroy();
			}
		}
	};

	Trim(ActiveSolids, TEXT("solid"));

	// --- And the same for water ---------------------------------------------
	//
	// STILL NEEDED, AND FOR HALF THE OLD REASON. Merging is no longer something
	// that can be missed -- two bodies of water that touch are one body because
	// their cells touch -- so the version of this failure where a player got a new
	// puddle per cast standing in the same spot is gone. What is left is real: a
	// player walking a field leaves a trail of genuinely disconnected puddles, and
	// nothing else bounds how many.
	//
	// CULLING IS TAKING THE WATER, not destroying the actor. A proxy is a handle;
	// destroying one would leave every drop exactly where it was with nothing
	// standing for it, so the budget would look like it worked and the field would
	// go on holding all of it.
	if (Regions.Num() > MaxBodiesOfEachKind)
	{
		TArray<AARPGFluidRegion*> Sorted = Regions;

		Sorted.Sort([](const AARPGFluidRegion& A, const AARPGFluidRegion& B)
			{
				return A.GetArea() < B.GetArea();
			});

		const int32 Excess = Regions.Num() - MaxBodiesOfEachKind;

		for (int32 Index = 0; Index < Excess && Index < Sorted.Num(); ++Index)
		{
			AARPGFluidRegion* Spent = Sorted[Index];

			if (!IsValid(Spent))
			{
				continue;
			}

			UE_LOG(LogARPGWorld, Verbose,
				TEXT("Over the water budget, so the smallest body goes."));

			// Whatever was riding it goes with it. A culled body leaving its floe
			// over dry ground would be the one visible artefact this economy could
			// produce.
			DropRiders(Spent);
			Spent->Drain();

			Regions.Remove(Spent);
			Spent->Destroy();
		}
	}
}

void UARPGFluidSurfaceSubsystem::StepSimulation(float DeltaTime)
{
	GatherViewers();
	TickFields(DeltaTime);
	TickWeather(DeltaTime);

	// AFTER THE WATER HAS MOVED, because what bodies there are is a question about
	// where the water is now -- two puddles that met during this step's flow are
	// one body by the time anything is asked about them.
	ReconcileRegions();
	EnforceBudget();
}

FARPGFluidField* UARPGFluidSurfaceSubsystem::FindField(UARPGFluidDefinition* Definition)
{
	if (!Definition)
	{
		return nullptr;
	}

	if (TUniquePtr<FARPGFluidField>* Existing = Fields.Find(Definition))
	{
		return Existing->Get();
	}

	TUniquePtr<FARPGFluidField> Field = MakeUnique<FARPGFluidField>();

	FARPGFluidFieldParams Params;
	Params.FlowRate = Definition->FlowRate;
	Params.YieldSlope = Definition->YieldSlope;
	Params.MinimumFilm = Definition->MinimumFilm;
	Params.WallSpeed = Definition->WallSpeed;
	Params.DepositDepth = Definition->Depth;

	// THE SAME NUMBER THE OLD CLIPPER USED. MaxDepositStep is what decided how far
	// the ground under a footprint could wander before the body stopped rather
	// than carrying on over it, and it means exactly the same thing here -- the
	// difference is that a field applies it per cell instead of per spoke.
	Params.MaxDepositDrop = MaxDepositStep;

	Field->Configure(FieldChunkSize, FieldResolution, Params);

	// THE SAME QUESTION A DEPOSIT ASKS, with the same exclusions -- a puddle must
	// not find ITSELF, or the floe beside it, and decide that is the ground. Bound
	// rather than called, so the field stays a plain class that a test can hand a
	// ramp to instead of a world.
	Field->SetBedProbe([this](const FVector2D& At, float& OutHeight)
		{
			// BARELY ABOVE WHERE IT WAS ASKED, and that is not a detail. The probe
			// traces DOWNWARD, so every centimetre of headroom is a centimetre in
			// which it can find a floor that is not the one being asked about --
			// and half a metre of it meant a spell cast on the ground under a
			// balcony traced up into the balcony and pooled on top of it.
			//
			// The headroom exists only so the trace does not start exactly on the
			// surface it is looking for. Anything more is the probe answering a
			// different question from the one the caller asked, and a heightfield
			// with layers has no way to tell that it did.
			return FindGroundAt(At, OutHeight + 20.f, OutHeight, nullptr);
		});

	FARPGFluidField* Raw = Field.Get();
	Fields.Add(Definition, MoveTemp(Field));

	return Raw;
}

const FARPGFluidField* UARPGFluidSurfaceSubsystem::FindField(UARPGFluidDefinition* Definition) const
{
	if (!Definition)
	{
		return nullptr;
	}

	const TUniquePtr<FARPGFluidField>* Existing = Fields.Find(Definition);
	return Existing ? Existing->Get() : nullptr;
}

double UARPGFluidSurfaceSubsystem::GetFieldVolume(FGameplayTag ElementTag) const
{
	const FARPGFluidField* Field = FindField(FindDefinition(ElementTag));
	return Field ? Field->GetTotalVolume() : 0.0;
}

bool UARPGFluidSurfaceSubsystem::IsFieldBlockedAt(FVector WorldPosition,
	FGameplayTag ElementTag) const
{
	const FARPGFluidField* Field = FindField(FindDefinition(ElementTag));
	return Field && Field->IsBlockedAt(FVector2D(WorldPosition.X, WorldPosition.Y));
}

bool UARPGFluidSurfaceSubsystem::IsFieldRoofedAt(FVector WorldPosition,
	FGameplayTag ElementTag) const
{
	const FARPGFluidField* Field = FindField(FindDefinition(ElementTag));
	return Field && Field->IsRoofedAt(FVector2D(WorldPosition.X, WorldPosition.Y));
}

float UARPGFluidSurfaceSubsystem::GetFieldDepthAt(FVector WorldPosition,
	FGameplayTag ElementTag) const
{
	const FARPGFluidField* Field = FindField(FindDefinition(ElementTag));
	return Field ? Field->SampleDepth(FVector2D(WorldPosition.X, WorldPosition.Y)) : 0.f;
}

void UARPGFluidSurfaceSubsystem::ResetFluid()
{
	for (TPair<TObjectPtr<UARPGFluidDefinition>, TUniquePtr<FARPGFluidField>>& Pair : Fields)
	{
		Pair.Value->Reset();
	}

	for (AARPGFluidRegion* Region : Regions)
	{
		if (IsValid(Region))
		{
			DropRiders(Region);
			Region->Destroy();
		}
	}

	Regions.Reset();
}

void UARPGFluidSurfaceSubsystem::RasterizeSolidsIntoFields()
{
	if (Fields.Num() == 0)
	{
		return;
	}

	for (TPair<TObjectPtr<UARPGFluidDefinition>, TUniquePtr<FARPGFluidField>>& Pair : Fields)
	{
		FARPGFluidField& Field = *Pair.Value;

		Field.ClearSolids();

		// GATHERED, THEN POURED, because pouring inside the loop would put water
		// into ground a slab further down the list is about to occupy -- and the
		// only symptom would be a puddle that keeps being shoved a second time.
		struct FSpill
		{
			FVector2D Where;
			float NearZ;
			float Reach;
			double Volume;
		};

		TArray<FSpill> Spills;

		for (AARPGSolidBody* Solid : ActiveSolids)
		{
			if (!IsValid(Solid))
			{
				continue;
			}

			const double FromThis = Solid->RasterizeInto(Field);

			if (FromThis <= 0.0)
			{
				continue;
			}

			// WHERE TO PUT IT BACK: the slab's own rim. PourVolume refuses blocked
			// cells, so a pour centred on the slab lands in a ring around it, which
			// is where water shoved aside by a wall actually goes.
			const FBox2D Outline = ARPGFluidGeometry::PolygonBounds(Solid->GetOutline());

			Spills.Add({
				Solid->GetWorldCentre(),
				Solid->GetSurfaceHeight(),
				static_cast<float>(Outline.GetExtent().Size()) + Field.GetCellSize() * 2.f,
				FromThis });
		}

		for (const FSpill& Spill : Spills)
		{
			Field.PourVolume(Spill.Where, Spill.NearZ, Spill.Volume, Spill.Reach);
		}
	}
}

void UARPGFluidSurfaceSubsystem::TickFields(float DeltaTime)
{
	if (Fields.Num() == 0)
	{
		return;
	}

	// BEFORE THE STEPS, not after: a slab that moved this tick has to be where it
	// is before any water is asked to flow around it, or the water settles into
	// ground that is about to be occupied and gets displaced right back out.
	RasterizeSolidsIntoFields();

	// ITS OWN ACCUMULATOR, because the field runs faster than the weather does and
	// the two rates have nothing to do with each other -- see FieldStepRate. The
	// base class owns the accumulator for the OUTER rate; this is the inner one.
	const float Interval = 1.f / FMath::Max(1.f, FieldStepRate);

	FieldAccumulator += DeltaTime;

	// CAPPED, so a hitch does not turn into a hundred catch-up steps and a longer
	// hitch. Water arriving slightly late after a stall is not something a player
	// can see; the frame it costs to pretend otherwise is.
	int32 Budget = 8;

	while (FieldAccumulator >= Interval && Budget-- > 0)
	{
		FieldAccumulator -= Interval;

		for (TPair<TObjectPtr<UARPGFluidDefinition>, TUniquePtr<FARPGFluidField>>& Pair : Fields)
		{
			Pair.Value->Step(Interval);
		}
	}

	if (Budget <= 0)
	{
		FieldAccumulator = 0.f;
	}
}

void UARPGFluidSurfaceSubsystem::TickWeather(float DeltaTime)
{
	// RAIN AND EVAPORATION ARE A DEPTH, NOT AN OFFSET. A pool grew and shrank by
	// pushing its outline out and in, because an outline was all it had -- which
	// meant rain filled a puddle by making it WIDER and a drying one narrowed
	// uniformly from every edge at once, including the edge against a wall.
	//
	// On a field it is what it actually is: so many centimetres on or off the
	// depth of every wet cell, everywhere. A shallow rim dries out first and the
	// deep middle lasts longest, which is what a drying puddle does and what the
	// polygon model could not express at any price.
	//
	// AND THIS IS THE ONE THING ALLOWED TO CHANGE THE TOTAL -- see
	// FARPGFluidField::ApplyWeather. The solver conserves mass exactly; weather is
	// where water is supposed to arrive from and go to.
	for (TPair<TObjectPtr<UARPGFluidDefinition>, TUniquePtr<FARPGFluidField>>& Pair : Fields)
	{
		const UARPGFluidDefinition* Definition = Pair.Key;

		if (!Definition)
		{
			continue;
		}

		const float Rate = bRaining
			? Definition->RainGrowthRate
			: -Definition->EvaporationRate;

		if (FMath::IsNearlyZero(Rate))
		{
			continue;
		}

		Pair.Value->ApplyWeather(Rate, DeltaTime);
	}

	// NOTHING TAKES A SLAB WITH TIME. Ambient melting used to live here -- a floe
	// thinned from both faces and retreated at the rim four times a second, and
	// every one of those ticks rebuilt its mesh and re-cooked its collision. It
	// went when solids stopped being heightfields: a slab is an outline extruded
	// to a thickness now, and an outline has nothing to thin. A floe stays until
	// something breaks it.
	//
	// The register is still swept, because a slab destroyed elsewhere leaves a
	// stale entry and nothing else would notice.
	for (int32 Index = ActiveSolids.Num() - 1; Index >= 0; --Index)
	{
		if (!IsValid(ActiveSolids[Index]))
		{
			ActiveSolids.RemoveAt(Index);
		}
	}
}

