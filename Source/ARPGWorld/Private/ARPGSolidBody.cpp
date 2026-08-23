// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGSolidBody.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGFluidGeometry.h"
#include "ARPGFluidDefinition.h"
#include "ARPGFluidSurfaceSubsystem.h"
#include "ARPGMagicElement.h"
#include "ARPGSolidDefinition.h"
#include "ARPGWorld.h"
#include "Components/BoxComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

namespace
{
	/** A repeatable wobble in [-0.5, 0.5) for one whole-numbered lattice point. */
	float Hashed(int32 X, int32 Y)
	{
		uint32 Hash = static_cast<uint32>(X) * 73856093u ^ static_cast<uint32>(Y) * 19349663u;
		Hash ^= Hash >> 13;
		Hash *= 0x5bd1e995u;
		Hash ^= Hash >> 15;
		return (Hash & 0xFFFFFFu) / static_cast<float>(0x1000000) - 0.5f;
	}

	/**
	 * Value noise: a hashed lattice with smooth interpolation between its points.
	 *
	 * SMOOTH RATHER THAN A BARE HASH, and that is the whole reason this is not two
	 * lines. Vertices land wherever the triangulator puts them, so two of them can
	 * be a centimetre apart; hashing their positions directly would give those two
	 * unrelated heights and the surface would come out as spikes rather than as
	 * rock. Interpolating a coarse lattice gives neighbouring points nearly the
	 * same answer and distant ones different ones, which is what a rough surface
	 * actually is.
	 */
	float ValueNoise(const FVector2D& At, float Scale)
	{
		const FVector2D Lattice = At / FMath::Max(1.f, Scale);

		const int32 X = FMath::FloorToInt32(Lattice.X);
		const int32 Y = FMath::FloorToInt32(Lattice.Y);

		const float TX = static_cast<float>(Lattice.X) - X;
		const float TY = static_cast<float>(Lattice.Y) - Y;

		// Smoothstep on each axis, so the surface has no creases along the lattice.
		const float SX = TX * TX * (3.f - 2.f * TX);
		const float SY = TY * TY * (3.f - 2.f * TY);

		const float Low = FMath::Lerp(Hashed(X, Y), Hashed(X + 1, Y), SX);
		const float High = FMath::Lerp(Hashed(X, Y + 1), Hashed(X + 1, Y + 1), SX);

		return FMath::Lerp(Low, High, SY);
	}
}

// ---------------------------------------------------------------------------

AARPGSolidBody::AARPGSolidBody()
{
	// THE ONE BODY THAT MOVES. A fluid is its ground height and never budges; a
	// floe rides a surface that is somewhere else every frame. Every frame rather
	// than on the weather tick, because settling and drifting are things you watch
	// -- 4Hz would step visibly -- and both are a few queries and a
	// SetActorLocation with no mesh rebuild behind them.
	PrimaryActorTick.bCanEverTick = true;

	SetNetUpdateFrequency(10.f);
}

void AARPGSolidBody::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// AN OUTLINE AND A FRAME, which is the whole of what a slab is now. The
	// heightfield this replaced was the heaviest payload in the game -- a
	// ten-metre floe was thousands of cells, run-length encoded and still
	// kilobytes, resent whenever any of it melted. A polygon is a handful of
	// vertices and only changes when something breaks the slab.
	DOREPLIFETIME(AARPGSolidBody, Definition);
	DOREPLIFETIME(AARPGSolidBody, Hole);
	DOREPLIFETIME(AARPGSolidBody, Bites);
	DOREPLIFETIME(AARPGSolidBody, SlabOrigin);
	DOREPLIFETIME(AARPGSolidBody, SlabYaw);
	DOREPLIFETIME(AARPGSolidBody, Draft);
}

void AARPGSolidBody::Setup(UARPGSolidDefinition* InDefinition, const TArray<FVector2D>& InRing,
	float InGroundHeight, const TArray<FVector2D>& InHole)
{
	Definition = InDefinition;
	GroundHeight = InGroundHeight;

	if (InDefinition && InDefinition->Element)
	{
		Volume->Element = InDefinition->Element;
	}

	// RECENTRED INTO ITS OWN FRAME. The ring arrives in world coordinates, because
	// it came from clipping one surface against another; from here on the slab
	// keeps its shape at its own origin and the actor's transform says where that
	// origin is. Drifting and turning then cost a transform rather than a rebuilt
	// polygon.
	SlabOrigin = ARPGFluidGeometry::PolygonCentroid(InRing);
	SlabYaw = 0.f;

	TArray<FVector2D> Local;
	Local.Reserve(InRing.Num());
	for (const FVector2D& Point : InRing)
	{
		Local.Add(Point - SlabOrigin);
	}

	// AND BROKEN UP, IF THIS IS ROCK. Nudging the vertices here rather than at
	// draw time is the whole point: it becomes the outline, so the shape you can
	// see, stand on and break is one polygon. A drawn edge that wandered off the
	// simulated one would be the exact class of bug this rewrite was for.
	if (InDefinition && InDefinition->Facets.Spread > 0.f && Local.Num() >= 3)
	{
		const float Spread = FMath::Min(InDefinition->Facets.Spread, 0.4f);

		TArray<FVector2D> Broken;
		Broken.Reserve(Local.Num());

		for (int32 At = 0; At < Local.Num(); ++At)
		{
			const FVector2D& Previous = Local[(At + Local.Num() - 1) % Local.Num()];
			const FVector2D& Next = Local[(At + 1) % Local.Num()];

			// Scaled by the gap to its neighbours, so a vertex on a long straight
			// edge moves further than one in a tight corner and neither can cross
			// what it sits between.
			const double Room = FMath::Min(
				FVector2D::Distance(Local[At], Previous),
				FVector2D::Distance(Local[At], Next)) * Spread;

			Broken.Add(Local[At] + FVector2D(
				Hashed(At, 11) * 2.f, Hashed(At, 29) * 2.f) * Room);
		}

		Local = MoveTemp(Broken);
	}

	Hole.Reset();
	Hole.Reserve(InHole.Num());
	for (const FVector2D& Point : InHole)
	{
		Hole.Add(Point - SlabOrigin);
	}

	SetRing(Local);
}

void AARPGSolidBody::AdoptOutline(UARPGSolidDefinition* InDefinition,
	const TArray<FVector2D>& InRing, const TArray<FVector2D>& InHole,
	const FVector& Where, float Yaw)
{
	Definition = InDefinition;
	GroundHeight = static_cast<float>(Where.Z);

	if (InDefinition && InDefinition->Element)
	{
		Volume->Element = InDefinition->Element;
	}

	// ALREADY IN A FRAME, and it keeps it. The ring handed in is the one the slab
	// was carrying, still centred on its own origin, so putting it down is a
	// matter of saying where that origin now is and which way it is pointing.
	FloatsOn = nullptr;
	bAground = true;
	Draft = 0.f;

	SlabOrigin = FVector2D(Where.X, Where.Y);
	SlabYaw = Yaw;

	Hole = InHole;
	SetRing(InRing);
}

// ---------------------------------------------------------------------------
// Shape
// ---------------------------------------------------------------------------

FVector2D AARPGSolidBody::GetWorldCentre() const
{
	return ToWorld(ARPGFluidGeometry::PolygonCentroid(Ring));
}

double AARPGSolidBody::SupportDistance(const FVector2D& From, const FVector2D& Direction) const
{
	double Furthest = 0.0;

	for (const FVector2D& Point : Ring)
	{
		Furthest = FMath::Max(Furthest, FVector2D::DotProduct(Point - From, Direction));
	}

	return Furthest;
}

bool AARPGSolidBody::ContainsPoint(FVector WorldPoint) const
{
	const FVector2D Local = ToLocal(FVector2D(WorldPoint.X, WorldPoint.Y));

	if (!ARPGFluidGeometry::PolygonContains(Ring, Local))
	{
		return false;
	}

	// A HOLE IS NOT PART OF THE SLAB. A liquid flows back over a gap cut in it and
	// so a pool has none; a solid keeps its, and a floe with one is a floe you can
	// fall through.
	return Hole.Num() < 3 || !ARPGFluidGeometry::PolygonContains(Hole, Local);
}

bool AARPGSolidBody::IsStandableAt(FVector WorldPoint) const
{
	return Definition && Definition->bStandable && ContainsPoint(WorldPoint);
}

bool AARPGSolidBody::IsSurfaceAt(const FVector2D& At) const
{
	return ContainsPoint(FVector(At.X, At.Y, 0.f));
}

TArray<FVector2D> AARPGSolidBody::GetSurfaceFootprint(const FVector2D& Centre, double Radius) const
{
	// IN WORLD SPACE, because whatever is asking is going to clip it against
	// something that lives out there. Everything else about a slab is in its own
	// frame; this is the one place that has to hand the outline back in the
	// world's.
	TArray<FVector2D> World;
	World.Reserve(Ring.Num());

	for (const FVector2D& Point : Ring)
	{
		World.Add(ToWorld(Point));
	}

	return World;
}

double AARPGSolidBody::DistanceToEdge(const FVector2D& World) const
{
	if (Ring.Num() < 3)
	{
		return 0.0;
	}

	// TO THE OUTLINE ITSELF, which a polygon can answer exactly. Nothing here is
	// approximated from a support direction any more -- the shape is the shape,
	// and the distance to it is a segment distance per edge.
	return FMath::Max(0.0, ARPGFluidGeometry::PolygonSignedDistance(Ring, ToLocal(World)));
}

float AARPGSolidBody::BiteDepthAt(const FVector2D& World) const
{
	if (Bites.Num() == 0)
	{
		return 0.f;
	}

	const FVector2D Local = ToLocal(World);

	// THE DEEPEST WINS rather than the sum, so two overlapping bowls read as one
	// wider crater instead of drilling twice as far where they happen to cross.
	float Deepest = 0.f;
	for (const FARPGSlabBite& Bite : Bites)
	{
		Deepest = FMath::Max(Deepest, Bite.DepthAt(Local));
	}

	// Never past the underside. Below that there is no material left to take, and
	// what happens instead is a hole -- see BreakAt.
	return FMath::Min(Deepest, Definition ? Definition->Thickness : 0.f);
}

float AARPGSolidBody::GetSurfaceLevelAt(const FVector2D& At) const
{
	// THE TOP HERE, which after a spell has landed on it is not one number: a bowl
	// cut into the middle is centimetres lower than the rim around it, and anything
	// standing in it is standing at the bottom.
	return GetSurfaceHeight() - BiteDepthAt(At);
}

double AARPGSolidBody::BreakAt(const FVector2D& Where, float Radius, float Depth)
{
	if (!Definition || Radius <= 0.f || Depth <= 0.f || Ring.Num() < 3)
	{
		return 0.0;
	}

	const float Thickness = Definition->Thickness;
	const FVector2D Local = ToLocal(Where);

	// DEEPENED RATHER THAN APPENDED when it lands on an existing bowl. Sustained
	// fire at one spot is what should break through, and a list that only ever grew
	// would spend its cap recording the same crater a dozen times.
	FARPGSlabBite* Existing = nullptr;
	for (FARPGSlabBite& Bite : Bites)
	{
		if (FVector2D::Distance(Bite.At, Local) < FMath::Max(Bite.Radius, Radius) * 0.5f)
		{
			Existing = &Bite;
			break;
		}
	}

	// WHAT IT HAD ALREADY TAKEN, so deepening a crater is charged only for the
	// material it takes THIS time. Otherwise a wall hit ten times would hand back
	// the same rock ten times over.
	const double Had = Existing
		? BowlVolume(Existing->Radius, Existing->Depth, Thickness)
		: 0.0;

	if (Existing)
	{
		Existing->Depth += Depth;
		Existing->Radius = FMath::Max(Existing->Radius, Radius);
	}
	else
	{
		FARPGSlabBite Bite;
		Bite.At = Local;
		Bite.Radius = Radius;
		Bite.Depth = Depth;
		Bites.Add(Bite);

		if (Bites.Num() > MaxBites)
		{
			// The shallowest is the one nobody would miss.
			int32 Faintest = 0;
			for (int32 At = 1; At < Bites.Num(); ++At)
			{
				if (Bites[At].Depth < Bites[Faintest].Depth)
				{
					Faintest = At;
				}
			}
			Bites.RemoveAt(Faintest);
		}
	}

	const FARPGSlabBite& Cut = Existing ? *Existing : Bites.Last();
	const double Removed = FMath::Max(0.0, BowlVolume(Cut.Radius, Cut.Depth, Thickness) - Had);

	// AND WHERE THE BOWL REACHED THE UNDERSIDE, the material is gone rather than
	// merely thinner -- so the outline has to lose it. A paraboloid of depth D over
	// radius R is at or past the thickness T within
	//
	//     r = R * sqrt(1 - T / D)
	//
	// which is nothing at all until the bowl is deeper than the slab is thick, and
	// grows from there. That is the same arithmetic that made a heightfield bowl
	// open a hole, with no second case for it.
	//
	// CUT WITHOUT ACCOUNTING, because the bowl volume above already counts what
	// went through -- BowlVolume clips at the thickness. Letting the cut deposit as
	// well would hand the same rock back twice.
	if (Cut.Depth > Thickness)
	{
		const float Through = Cut.Radius * FMath::Sqrt(1.f - Thickness / Cut.Depth);

		if (Through >= 1.f)
		{
			CutRegion(ARPGFluidGeometry::MakeCircle(ToWorld(Cut.At), Through));
		}
	}

	return Removed;
}

double AARPGSolidBody::GetArea() const
{
	const double Outline = ARPGFluidGeometry::PolygonArea(Ring);

	return Hole.Num() >= 3
		? FMath::Max(0.0, Outline - ARPGFluidGeometry::PolygonArea(Hole))
		: Outline;
}

double AARPGSolidBody::GetMinimumArea() const
{
	return Definition ? Definition->MinimumArea : 0.0;
}

float AARPGSolidBody::GetSurfaceOffset() const
{
	return Definition ? Definition->Thickness : 0.f;
}

float AARPGSolidBody::GetSurfaceEnergyDensity() const
{
	return Definition ? Definition->EnergyPerArea : 0.f;
}

bool AARPGSolidBody::ConsumeSurfaceArea(double Area)
{
	if (Area <= 0.0)
	{
		return false;
	}

	const double Current = GetArea();
	const double Remaining = FMath::Max(0.0, Current - Area);

	if (Remaining < GetMinimumArea())
	{
		return true;
	}

	// THE HOLE WIDENS AS THE OUTLINE SHRINKS, which is the one place a solid and a
	// liquid genuinely differ about erosion. A liquid flows back over a gap, so a
	// pool has none to widen; a slab worn from both sides thins into a ring, and
	// the gap it froze around is part of what it is.
	if (Hole.Num() >= 3)
	{
		const double HoleArea = ARPGFluidGeometry::PolygonArea(Hole);
		const double Widened = HoleArea + (Current - Remaining) * 0.5;

		if (Widened >= Remaining)
		{
			return true; // the gap has eaten the slab
		}

		Hole = ARPGFluidGeometry::GrowToArea(Hole, Widened);
	}

	ReturnBrokenFluid((Current - Remaining) * Definition->Thickness);

	SetRing(ARPGFluidGeometry::ShrinkToArea(Ring, Remaining));
	return false;
}

double AARPGSolidBody::BowlVolume(float Radius, float Depth, float Thickness)
{
	if (Radius <= 0.f || Depth <= 0.f || Thickness <= 0.f)
	{
		return 0.0;
	}

	const double R2 = static_cast<double>(Radius) * Radius;

	// A paraboloid d(r) = D(1 - r^2/R^2) integrated over the disc is half the
	// cylinder that contains it.
	if (Depth <= Thickness)
	{
		return PI * R2 * Depth * 0.5;
	}

	// Past the underside there is nothing left to take, so the bowl is clipped:
	// integrating min(d(r), T) instead gives a full cylinder less the dimple that
	// is still above the floor. Tends to the whole cylinder as the bowl deepens.
	return PI * R2 * Thickness * (1.0 - Thickness / (2.0 * Depth));
}

void AARPGSolidBody::ReturnBrokenFluid(double SolidVolume)
{
	// Null BreaksInto is correct for obsidian: rock that formed on lava is not
	// frozen lava, and breaking it releases nothing.
	if (SolidVolume <= 0.0 || !Definition || !Definition->BreaksInto)
	{
		return;
	}

	UARPGFluidDefinition* Fluid = Definition->BreaksInto;

	UARPGFluidSurfaceSubsystem* Fluids =
		GetWorld() ? GetWorld()->GetSubsystem<UARPGFluidSurfaceSubsystem>() : nullptr;

	// SAID FIRST, AND SAID WHATEVER HAPPENS NEXT. The reaction's own product would
	// otherwise deposit this same fluid a second time when its discharge lands --
	// and the product's deposit is sized by the SPELL rather than by the material
	// removed, so what the player saw was a chip out of a wall and a lake of lava.
	// Fire + earth is one body of lava, not two.
	if (Fluids)
	{
		Fluids->NoteFluidReturned(Fluid->GetElementTag());
	}

	// MASS IS WHAT IS CONSERVED, not volume. Rock is denser than the lava it melts
	// into, so a cubic metre of wall becomes the volume of lava that weighs the
	// same -- the same two densities that decide whether the slab floats.
	const double FluidVolume = SolidVolume
		* Definition->Density / FMath::Max(KINDA_SMALL_NUMBER, Fluid->Density);

	if (!Fluids || FluidVolume <= 0.0)
	{
		return;
	}

	const FVector2D Where = bHasPendingContact ? PendingContact : GetWorldCentre();

	// BACK INTO WHATEVER IT IS RIDING, and the slab never learns which kind of
	// thing that is: a lake takes it and nothing appears, a puddle takes it by
	// growing its outline.
	IARPGElementalSurface* Riding =
		IsValid(FloatsOn.GetObject()) ? FloatsOn.GetInterface() : nullptr;

	if (Riding && Riding->IsSurfaceAt(Where) && Riding->AbsorbSurfaceVolume(FluidVolume))
	{
		return;
	}

	// THE BED, not the waterline: fluid lies on the bottom. For a slab on dry
	// ground the two are the same and either would do.
	const float Bed = Riding ? Riding->GetSurfaceBedAt(Where) : GroundHeight;

	// BY VOLUME, so the pool that appears is the size of the hole that was cut. A
	// radius picked from the plan area alone ignored the fluid's own depth and the
	// two densities, and was wrong by whatever those came to.
	Fluids->ReturnFluid(Where, Bed, FluidVolume, Fluid);
}

bool AARPGSolidBody::ConsumeSurfaceRegion(const TArray<FVector2D>& Region)
{
	// THE GROUND ACTUALLY LOST, not the region asked for. A hit at the corner of a
	// slab is mostly off the edge of it, and depositing for the whole disc would
	// hand back lava the wall never had.
	const double Before = GetArea();
	const bool bFinished = CutRegion(Region);

	ReturnBrokenFluid((Before - GetArea())
		* (Definition ? Definition->Thickness : 0.f));

	return bFinished;
}

bool AARPGSolidBody::CutRegion(const TArray<FVector2D>& Region)
{
	const double Taken = ARPGFluidGeometry::PolygonArea(Region);

	if (Taken <= 0.0 || Ring.Num() < 3)
	{
		return false;
	}

	// INTO THE SLAB'S OWN FRAME, because the outline is in it and the region is
	// not: the contact came from the world, where the spell happened.
	TArray<FVector2D> Local;
	Local.Reserve(Region.Num());
	for (const FVector2D& Point : Region)
	{
		Local.Add(ToLocal(Point));
	}

	TArray<FVector2D> Cut;
	TArray<TArray<FVector2D>> Gaps;
	ARPGFluidGeometry::SubtractWithHoles(Ring, Local, Cut, Gaps);

	if (Cut.Num() < 3 || ARPGFluidGeometry::PolygonArea(Cut) < GetMinimumArea())
	{
		return true;
	}

	// AND THE HOLE IT OPENED, which is the whole reason a slab does not use the
	// base class's version. A hit in the middle of a wall leaves the outline
	// untouched and expresses itself entirely as a hole; drop that and the spell
	// did nothing anyone could see.
	//
	// THE LARGEST, AND ONLY ONE. A slab draws a single gap -- see Hole -- so a
	// second hit that opens another keeps whichever is worth looking through.
	const TArray<FVector2D>* Largest = Hole.Num() >= 3 ? &Hole : nullptr;
	double LargestArea = Largest ? ARPGFluidGeometry::PolygonArea(*Largest) : 0.0;

	for (const TArray<FVector2D>& Gap : Gaps)
	{
		const double GapArea = ARPGFluidGeometry::PolygonArea(Gap);
		if (GapArea > LargestArea)
		{
			LargestArea = GapArea;
			Largest = &Gap;
		}
	}

	const TArray<FVector2D> Kept = Largest ? *Largest : TArray<FVector2D>();

	Hole = Kept;
	SetRing(Cut);
	return false;
}

void AARPGSolidBody::OnElementalReaction_Implementation(float Consumed, float Remaining,
	UARPGMagicElement* Product)
{
	const float Density = GetSurfaceEnergyDensity();

	if (Consumed <= 0.f || Density <= 0.f)
	{
		bHasPendingContact = false;
		return;
	}

	const double Ground = Consumed / Density;

	// A BOWL WHERE IT LANDED, if the solver left a contact.
	//
	// THE RADIUS IS AUTHORED AND THE DEPTH IS EARNED, which is the pair that makes
	// a hit read as a hit: BreakRadius decides how broad the crater is and the
	// energy decides how far in it goes, so narrow and deep punches through while
	// wide and shallow dishes the surface. A bowl removes about half the volume of
	// the cylinder around it, hence the two.
	//
	// Without a contact -- anything that spent this slab without touching a point
	// on it -- there is nowhere to put a bowl, and taking the ground off the
	// outline is the only honest answer left.
	bool bFinished = false;

	if (bHasPendingContact)
	{
		const float Radius = FMath::Max(1.f, Definition->BreakRadius);
		const float Depth = static_cast<float>(2.0 * Ground / (PI * Radius * Radius))
			* Definition->Thickness;

		const double Removed = BreakAt(PendingContact, Radius, Depth);

		// EQUAL TO WHAT THE BOWL TOOK. The lava a fireball leaves is the rock it
		// melted, by mass -- not a pool sized by how big the spell was.
		ReturnBrokenFluid(Removed);

		bHasPendingContact = false;

		// A dish takes no ground, so a slab is finished only once the bowls have
		// eaten enough of its outline to leave nothing worth standing on.
		bFinished = Ring.Num() < 3 || GetArea() < GetMinimumArea();

		if (!bFinished)
		{
			RebuildFromRing();
		}
	}
	else
	{
		bFinished = ConsumeSurfaceArea(Ground);
	}

	if (bFinished)
	{
		if (UARPGFluidSurfaceSubsystem* Fluids =
				GetWorld() ? GetWorld()->GetSubsystem<UARPGFluidSurfaceSubsystem>() : nullptr)
		{
			Fluids->RetireBody(this);
		}
	}
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

UMaterialInterface* AARPGSolidBody::ResolveSurfaceMaterial() const
{
	return Definition ? Definition->SurfaceMaterial.LoadSynchronous() : nullptr;
}

ARPGFluidGeometry::FBedSampler AARPGSolidBody::GetBedSampler() const
{
	if (!Definition || Definition->Facets.Relief <= 0.f)
	{
		return {};
	}

	// THE WHOLE COLUMN MOVES. BuildSlabMesh lifts both caps by whatever this
	// returns, so the slab keeps exactly the thickness it was given and the
	// underside breaks up with the top -- see FARPGSurfaceFacets::Relief.
	//
	// SAMPLED IN THE SLAB'S OWN FRAME, so the rock does not swim across its own
	// surface when the slab drifts or is turned by a current.
	const float Relief = Definition->Facets.Relief;
	const float Grain = FMath::Max(1.f, Definition->Facets.Grain);
	const FVector2D Origin = SlabOrigin;
	const float Yaw = SlabYaw;

	return [Relief, Grain, Origin, Yaw](const FVector2D& World) -> double
	{
		const FVector2D Local = (World - Origin).GetRotated(-Yaw);
		return ValueNoise(Local, Grain) * Relief;
	};
}

ARPGFluidGeometry::FBedSampler AARPGSolidBody::GetTopRelief() const
{
	if (Bites.Num() == 0)
	{
		return {};
	}

	// DOWN, never up: a bowl takes material away. Captured by value because the
	// mesher may outlive the call and the slab's own state can change underneath it.
	TArray<FARPGSlabBite> Cut = Bites;
	const float Thickness = Definition ? Definition->Thickness : 0.f;
	const FVector2D Origin = SlabOrigin;
	const float Yaw = SlabYaw;

	return [Cut, Thickness, Origin, Yaw](const FVector2D& World) -> double
	{
		const FVector2D Local = (World - Origin).GetRotated(-Yaw);

		float Deepest = 0.f;
		for (const FARPGSlabBite& Bite : Cut)
		{
			Deepest = FMath::Max(Deepest, Bite.DepthAt(Local));
		}

		return -FMath::Min(Deepest, Thickness);
	};
}

double AARPGSolidBody::GetBedDetailSpacing() const
{
	// NO RELIEF, NO INTERIOR POINTS. A flat slab is a triangulated outline and
	// nothing else, which for ice is both the honest shape and the cheap one --
	// a whole floe is a few dozen triangles. Rock asks for a grid of interior
	// vertices so its top has somewhere to bend, and that is what it costs to not
	// read as a slab of glass.
	if (!Definition)
	{
		return 0.0;
	}

	double Spacing = 0.0;

	// Half the grain, so every bump gets vertices either side of it rather than
	// being sampled once and lost between them.
	if (Definition->Facets.Relief > 0.f)
	{
		Spacing = FMath::Max(10.f, Definition->Facets.Grain * 0.5f);
	}

	// AND FINE ENOUGH TO HOLD A BOWL. A dish is a height function sampled at
	// vertices, so it exists only as far as there are vertices inside it to carry
	// it -- a third of the radius puts several across every crater, which is what
	// makes it read as a bowl rather than as a dent.
	if (Bites.Num() > 0)
	{
		const double ForBites = FMath::Max(10.f, Definition->BreakRadius / 3.f);
		Spacing = Spacing > 0.0 ? FMath::Min(Spacing, ForBites) : ForBites;
	}

	return Spacing;
}

void AARPGSolidBody::RebuildFromRing()
{
	if (!Definition || Ring.Num() < 3)
	{
		ARPGFluidGeometry::BuildSlabMesh(Surface, TArray<FVector2D>(), TArray<FVector2D>(),
			FVector2D::ZeroVector, 0.f, 0.f);
		Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	// PLACED BY ITS OWN FRAME rather than by its outline's bounding box, which is
	// what the base class does and what a body that never moves can afford. A slab
	// keeps its polygon still and moves the actor, so the actor is what carries
	// the position and the yaw.
	const FVector2D World = ToWorld(FVector2D::ZeroVector);

	SetActorLocationAndRotation(
		FVector(World.X, World.Y, GroundHeight + GetVerticalOffset()),
		FRotator(0.f, SlabYaw, 0.f));

	const FVector2D Extent = ARPGFluidGeometry::PolygonBounds(Ring).GetExtent();

	Bounds->SetBoxExtent(FVector(
		FMath::Max(1.f, static_cast<float>(Extent.X)),
		FMath::Max(1.f, static_cast<float>(Extent.Y)),
		FMath::Max(1.f, Definition->Thickness * 4.f + 100.f)));

	Volume->SurfaceHeightOffset = Definition->Thickness;

	// AT THE FRAME'S OWN ORIGIN, not the outline's centre. The mesh is emitted in
	// the component's local space and the component sits at SlabOrigin, so zero is
	// where the two agree.
	ARPGFluidGeometry::BuildSlabMesh(Surface, Ring, Hole, FVector2D::ZeroVector,
		/*BottomZ=*/0.f, /*TopZ=*/Definition->Thickness,
		GetBedSampler(), GetBedDetailSpacing(), GetTopRelief());

	if (UMaterialInterface* Material = ResolveSurfaceMaterial())
	{
		Surface->SetMaterial(0, Material);
	}

	// A slab is a thing you stand on and a body a spell can break, but never an
	// ambient source and never a conductor.
	Volume->SetEnergy(GetArea() * GetSurfaceEnergyDensity());
	Volume->bReservoir = false;
	Volume->bAmbientSource = false;
	Volume->Conductivity = 0.f;

	if (!Definition->bStandable)
	{
		Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	// THE DRAWN SURFACE IS THE WALKABLE ONE. Nothing approximates the other.
	Surface->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Surface->SetCollisionObjectType(ECC_WorldDynamic);
	Surface->SetCollisionResponseToAllChannels(ECR_Block);
	Surface->EnableComplexAsSimpleCollision();

	// COOKING IS THE EXPENSIVE PART, and it is worth nothing for a slab nobody can
	// reach. Far rarer than it used to be -- a slab is rebuilt only when something
	// breaks it, where the heightfield rebuilt four times a second the whole time
	// it was melting -- but a wall being worn down by sustained fire still asks
	// repeatedly.
	const UARPGFluidSurfaceSubsystem* Fluids =
		GetWorld() ? GetWorld()->GetSubsystem<UARPGFluidSurfaceSubsystem>() : nullptr;

	if (!Fluids || Fluids->IsSignificantAt(GetActorLocation()))
	{
		Surface->UpdateCollision(/*bOnlyIfPending=*/false);
	}
}

// ---------------------------------------------------------------------------
// Floating
// ---------------------------------------------------------------------------

int32 AARPGSolidBody::CountOccupants() const
{
	const UWorld* World = GetWorld();
	if (!World || Ring.Num() < 3)
	{
		return 0;
	}

	// Measured in the slab's own frame, then the box that sweeps for pawns is
	// placed in the world. Generous either way: it is a broadphase, and
	// IsStandableAt is what actually decides.
	const FVector2D Extent = ARPGFluidGeometry::PolygonBounds(Ring).GetExtent();
	const FVector2D Centre = GetWorldCentre();
	const float Top = GetSurfaceHeight();

	// A SHALLOW SLICE ABOVE THE SLAB, because what is standing on it is not
	// overlapping it -- the mesh blocks, and a blocking contact generates no
	// overlap. Anything with feet in this slice is a candidate; the polygon test
	// below decides. A box centred on the slab instead would count whoever was
	// swimming under it.
	static constexpr float StandingSlice = 120.f;

	TArray<FOverlapResult> Overlaps;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ARPGFloeOccupants), /*bTraceComplex=*/false);
	Params.AddIgnoredActor(this);

	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_Pawn);

	// THE RETURN VALUE IS NOT WHETHER ANYTHING WAS FOUND. It reports whether any
	// hit BLOCKED, and a pawn capsule set to query-only merely touches -- so an
	// early return on it threw away a full array of overlaps and reported an empty
	// floe with someone standing on it.
	World->OverlapMultiByObjectType(Overlaps,
		FVector(Centre.X, Centre.Y, Top + StandingSlice * 0.5f),
		FQuat::Identity, ObjectParams,
		FCollisionShape::MakeBox(FVector(Extent.X, Extent.Y, StandingSlice * 0.5f)), Params);

	TSet<const AActor*> Counted;

	for (const FOverlapResult& Overlap : Overlaps)
	{
		const AActor* Actor = Overlap.OverlapObjectHandle.FetchActor<AActor>();
		if (!Actor)
		{
			continue;
		}

		// THE SAME TRUTH standing on it uses. Inside the outline and not down a
		// hole -- the box is broadphase, and a pawn in the box but off the floe is
		// in the water beside it, not aboard.
		if (IsStandableAt(Actor->GetActorLocation()))
		{
			Counted.Add(Actor);
		}
	}

	return Counted.Num();
}

float AARPGSolidBody::ComputeTargetDraft() const
{
	if (!Definition || !FloatsOn)
	{
		return 0.f;
	}

	// THE FLUID'S OWN DENSITY, asked rather than assumed. Nothing here names water
	// or ice: a crust on lava and a floe on a pond are the same two numbers
	// compared, and which of them floats is data.
	const float FluidDensity = FloatsOn->GetSurfaceDensity();

	const double Area = GetArea();
	if (Area <= 0.0 || FluidDensity <= 0.f)
	{
		return 0.f;
	}

	// ARCHIMEDES. A floating body displaces its own weight, so the depth it rides
	// at is mass over the fluid it has to push aside:
	//
	//     Draft = (SlabMass + LoadMass) / (FluidDensity * Area)
	//
	// The slab's own term reduces to Thickness * Density / FluidDensity, with the
	// area cancelling -- which is why a big slab and a small one of the same stuff
	// ride at the same depth, and correctly so. The load's term does NOT cancel,
	// so a wider slab takes a person's weight better. Both fall out of the
	// equation rather than being arranged.
	const float Thickness = Definition->Thickness;
	const float SlabDraft = Thickness * (Definition->Density / FluidDensity);

	const float LoadMass = OccupantCount * Definition->OccupantMass * Definition->LoadResponse;
	const float LoadDraft = LoadMass / (FluidDensity * static_cast<float>(Area));

	// Never further than under. Past this the slab is swamped, and letting it keep
	// sinking would drag whoever is standing on it through the floor.
	return FMath::Min(SlabDraft + LoadDraft, Thickness);
}

bool AARPGSolidBody::HasRoomToward(const FVector2D& Direction) const
{
	if (!FloatsOn || Ring.Num() < 3)
	{
		return false;
	}

	// Clear fluid this far past the edge counts as room.
	static constexpr double Clearance = 50.0;

	const FVector2D Centre = ARPGFluidGeometry::PolygonCentroid(Ring);
	const double Reach = SupportDistance(Centre, DirToLocal(Direction));

	// The probe goes back out into the world, because what it asks -- is there
	// still water over there -- is a question about the world and not the slab.
	return FloatsOn->IsSurfaceAt(ToWorld(Centre) + Direction * (Reach + Clearance));
}

void AARPGSolidBody::UpdateAnchoring()
{
	bAnchored = false;

	if (!FloatsOn || Ring.Num() < 3)
	{
		return;
	}

	// WEDGED, not merely touching. A floe pushed against one bank is still a raft
	// -- the current can turn it, slide it along, work it free. One that reaches
	// both banks at once is a plug, and nothing short of breaking it will move it.
	static const FVector2D Axes[2] = { FVector2D(1, 0), FVector2D(0, 1) };

	for (const FVector2D& Axis : Axes)
	{
		if (!HasRoomToward(Axis) && !HasRoomToward(-Axis))
		{
			bAnchored = true;
			return;
		}
	}
}

void AARPGSolidBody::BeginBuried(float Depth)
{
	Draft = FMath::Max(0.f, Depth);

	// AND PUT IT THERE, rather than only recording that it should be. Setup has
	// already placed the slab at its resting height, so setting the number alone
	// would leave it standing in full view until the first tick moved it down.
	const FVector2D World = ToWorld(FVector2D::ZeroVector);
	SetActorLocation(FVector(World.X, World.Y, GroundHeight - Draft));
}

void AARPGSolidBody::GroundOnBed(float BedHeight)
{
	FloatsOn = nullptr;
	bAground = true;

	// DRAFT IS HOW FAR UNDER ITS RESTING HEIGHT THE SLAB IS SITTING, and a slab on
	// the floor is at its resting height by definition.
	Draft = 0.f;
	GroundHeight = BedHeight;

	const FVector2D World = ToWorld(FVector2D::ZeroVector);
	SetActorLocation(FVector(World.X, World.Y, GroundHeight));
}

void AARPGSolidBody::Rise(float DeltaTime)
{
	// DRAFT ALREADY MEANS THIS. For a floe it is how far under the waterline the
	// slab is riding; for a slab coming out of the ground it is how much of it is
	// still buried. One number, and the actor is already placed at
	// GroundHeight - Draft, so pulling it to zero IS the rise.
	if (FMath::IsNearlyZero(Draft) || !Definition)
	{
		return;
	}

	Draft = FMath::FInterpTo(Draft, 0.f, DeltaTime, FMath::Max(0.01f, Definition->RiseSpeed));

	// Snapped rather than approached forever, because FInterpTo is asymptotic and
	// a slab a tenth of a millimetre short of home would tick, move and dirty its
	// replicated draft for the rest of the level's life.
	if (FMath::IsNearlyZero(Draft, 0.1f))
	{
		Draft = 0.f;
	}

	const FVector2D World = ToWorld(FVector2D::ZeroVector);
	SetActorLocation(FVector(World.X, World.Y, GroundHeight - Draft));
}

void AARPGSolidBody::Spin(float DeltaTime, const FVector2D& Centre, const FVector2D& Flow)
{
	if (!Definition || Definition->SpinResponse <= 0.f || Flow.IsNearlyZero())
	{
		return;
	}

	// THE SHEAR ACROSS THE SLAB IS WHAT TURNS IT. A current that is faster on one
	// flank than the other puts a couple on anything floating in it, and that is
	// the whole of why real ice turns as it goes. Two samples, one either side.
	const FVector2D Along = Flow.GetSafeNormal();
	const FVector2D Across(-Along.Y, Along.X);

	const double Half = FMath::Max(50.0,
		SupportDistance(ARPGFluidGeometry::PolygonCentroid(Ring), DirToLocal(Across)));

	const FVector2D Left = FloatsOn->GetSurfaceFlowAt(Centre + Across * Half);
	const FVector2D Right = FloatsOn->GetSurfaceFlowAt(Centre - Across * Half);

	// Only the component ALONG the current differs in a way that turns you; a
	// difference across it is the flow converging, which shoves rather than spins.
	const double Shear = FVector2D::DotProduct(Left - Right, Along) / (2.0 * Half);

	SlabYaw = FMath::UnwindDegrees(SlabYaw + static_cast<float>(
		FMath::RadiansToDegrees(Shear) * Definition->SpinResponse * DeltaTime));
}

void AARPGSolidBody::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Server only. The draft replicates as a result and the frame carries the
	// drift, so a client that simulated its own would be fighting both.
	if (!HasAuthority() || !Definition || Ring.Num() < 3)
	{
		return;
	}

	// ROOTED, so there is nothing to float on and nothing to work out. A slab
	// raised out of the ground has no FloatsOn: it does not settle, it does not
	// drift, and every query below would be asked of a surface that is not there.
	//
	// IsValid rather than a null check: an actor destroyed this frame is not
	// garbage collected until later, so a floe whose pool has just gone still
	// points at it, and being rooted is the right answer for that too.
	if (!IsValid(FloatsOn.GetObject()))
	{
		Rise(DeltaTime);
		return;
	}

	// NOBODY NEAR IT, NOBODY PAYING FOR IT. Settling and drifting exist entirely
	// for someone watching or standing on it.
	if (const UARPGFluidSurfaceSubsystem* Fluids = GetWorld()->GetSubsystem<UARPGFluidSurfaceSubsystem>())
	{
		if (!Fluids->IsSignificantAt(GetActorLocation()))
		{
			Surface->ComponentVelocity = FVector::ZeroVector;
			return;
		}
	}

	const FVector2D Centre = GetWorldCentre();

	// Polled rather than counted every frame: standing on a floe is something
	// people do for seconds at a time, and the query is an overlap sweep.
	OccupantPoll -= DeltaTime;
	if (OccupantPoll <= 0.f)
	{
		OccupantPoll = OccupantPollInterval;
		OccupantCount = CountOccupants();
	}

	GroundHeight = FloatsOn->GetSurfaceLevelAt(Centre);

	// DENSER THAN WHAT IT FORMED ON MEANS IT DOES NOT FLOAT. One comparison, in
	// the same Archimedes the floes use, reached without anything knowing it is
	// rock.
	bAground = Definition->Density >= FloatsOn->GetSurfaceDensity();

	const float TargetDraft = bAground
		? GroundHeight - FloatsOn->GetSurfaceBedAt(Centre)
		: ComputeTargetDraft();

	Draft = FMath::FInterpTo(Draft, TargetDraft, DeltaTime,
		FMath::Max(0.01f, Definition->SettleSpeed));

	// Sitting on the bottom, or braced on both banks. Either way the current has
	// nothing to push against -- an aground slab still bobs, because the water
	// under it is still there, but it does not travel.
	FVector2D Step = FVector2D::ZeroVector;
	FVector2D Flow = FVector2D::ZeroVector;

	if (!bAground && !bAnchored)
	{
		// LAGS THE CURRENT rather than matching it: a heavy slab against a fast
		// river does not travel at the water's speed.
		Flow = FloatsOn->GetSurfaceFlowAt(Centre) * Definition->DriftResponse;
		Step = Flow * DeltaTime;
	}

	// NOT OFF THE END OF ITS OWN WATER. One containment probe, and refusing the
	// step is what stops a floe beaching itself on a bank the anchor test did not
	// catch because the slab only reaches the shore on one side.
	const bool bTravels = !Step.IsNearlyZero() && HasRoomToward(Step.GetSafeNormal());

	if (bTravels)
	{
		// THE FRAME SLIDES, not the polygon. Drifting is one vector add.
		SlabOrigin += Step;

		// AND IT TURNS. A floe that slides down a river without ever spinning is
		// the tell that it is a shape being moved rather than an object, and the
		// river already knows enough to fix it -- see Spin.
		Spin(DeltaTime, Centre, Flow);
	}

	// SO IT CARRIES THE PLAYER. A character standing on a kinematic base is moved
	// by UCharacterMovementComponent's based movement, and the base's velocity is
	// what it imparts when they jump off. Without this the ice slides out from
	// under them, which is worse than not drifting at all.
	Surface->ComponentVelocity = bTravels ? FVector(Flow.X, Flow.Y, 0.f) : FVector::ZeroVector;

	const FVector2D Moved = ToWorld(FVector2D::ZeroVector);
	SetActorLocationAndRotation(
		FVector(Moved.X, Moved.Y, GroundHeight - Draft),
		FRotator(0.f, SlabYaw, 0.f));
}
