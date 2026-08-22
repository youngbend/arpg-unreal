// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGSolidBody.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGFluidDefinition.h"
#include "ARPGFluidGeometry.h"
#include "ARPGFluidSurfaceSubsystem.h"
#include "ARPGSolidDefinition.h"
#include "ARPGWorld.h"
#include "Components/BoxComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

// ---------------------------------------------------------------------------
// Solid
// ---------------------------------------------------------------------------

AARPGSolidBody::AARPGSolidBody()
{
	// THE ONE BODY THAT MOVES. A fluid is its ground height and never budges; a
	// floe rides a surface that is somewhere else every frame. Every frame rather
	// than on the weather tick, because settling and drifting are things you watch
	// -- 4Hz would step visibly -- and both are a few queries and a SetActorLocation
	// with no mesh rebuild behind them.
	PrimaryActorTick.bCanEverTick = true;

	// The outline replicates at its own rate while drifting; the transform between
	// those is the client's to smooth.
	SetNetUpdateFrequency(10.f);

	// WHAT HAS MELTED, drawn separately from what it melted off. See the header.
	Film = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("Film"));
	Film->SetupAttachment(GetRootComponent());
	Film->SetMobility(EComponentMobility::Movable);

	// NEVER. The rock under it is what you stand on, and a film with collision
	// would have a character walking on lava rather than through it.
	Film->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// Same reach as the slab's own surface: a film is smaller and fainter than
	// the thing it lies on, so anything that culls the rock should have culled
	// this first.
	Film->SetCullDistance(15000.f);
}

UPrimitiveComponent* AARPGSolidBody::GetFilmComponent() const
{
	return Film;
}

int32 AARPGSolidBody::GetFilmTriangleCount() const
{
	if (!Film)
	{
		return 0;
	}

	int32 Count = 0;
	Film->ProcessMesh([&Count](const UE::Geometry::FDynamicMesh3& Mesh)
	{
		Count = Mesh.TriangleCount();
	});

	return Count;
}

void AARPGSolidBody::RebuildFilm()
{
	if (!Film)
	{
		return;
	}

	// DRY IS THE COMMON CASE and costs one cached comparison -- HasWet is a total,
	// not a sweep. An earth wall nobody has hit pays that for the whole level.
	// Still called rather than skipped, because the tick AFTER the last of the
	// film drains is the one that has to clear the mesh.
	const UARPGFluidDefinition* Fluid = Definition ? Definition->MeltsInto : nullptr;

	ARPGFluidGeometry::BuildFilmMesh(Film, Field, Field.SolidCentroid(),
		Fluid ? Fluid->MinimumFilm : 0.f,
		Definition ? Definition->Facets : FARPGSurfaceFacets(), WallRuns);

	// THE FLUID'S MATERIAL, not the slab's: what is running down a pillar of earth
	// is lava, and drawing it in rock would make the whole point of it invisible.
	// Re-applied on every rebuild for the same reason the slab's is -- on a client
	// the definition arrives by replication and may land after the first film.
	if (Fluid)
	{
		if (UMaterialInterface* Material = Fluid->SurfaceMaterial.LoadSynchronous())
		{
			Film->SetMaterial(0, Material);
		}
	}
}

void AARPGSolidBody::Setup(UARPGSolidDefinition* InDefinition, const TArray<FVector2D>& InRing,
	float InGroundHeight)
{
	Definition = InDefinition;
	GroundHeight = InGroundHeight;

	// BUILT IN WORLD COORDINATES AND THEN CALLED LOCAL. The ring handed in is a
	// world polygon -- it came from clipping one surface against another -- so the
	// field's frame starts out coincident with the world and the mapping is the
	// identity. Everything that moves the slab afterwards moves the FRAME, and the
	// cells never learn that anything happened.
	FieldOrigin = FVector2D::ZeroVector;
	FieldYaw = 0.f;

	if (Definition && Definition->Element)
	{
		Volume->Element = Definition->Element;
	}

	// THE OUTLINE IS ONLY THE SEED. Where the two things overlapped is a genuinely
	// two-dimensional question and stays a polygon clip; what the slab becomes
	// afterwards -- melted at an angle, refrozen at a lower level, holed through --
	// is not, and that is what the field is for.
	Field.BuildFrom(InRing, Definition ? Definition->CellSize : 20.f,
		Definition ? Definition->Thickness : 30.f);

	RebuildFromRing();
}

float AARPGSolidBody::GetSurfaceOffset() const
{
	return Definition ? Definition->Thickness : 0.f;
}

float AARPGSolidBody::GetSurfaceLevelAt(const FVector2D& At) const
{
	if (!Field.IsValidField())
	{
		return GetSurfaceHeight();
	}

	// FROM THE FIELD, in the field's own frame. Off the slab entirely TopAt
	// answers zero, which as a height would be the world origin rather than
	// "nothing here" -- so the nominal top stands in for anywhere the field has
	// nothing to say.
	const float Local = Field.TopAt(ToField(At));

	return Local > 0.f
		? GroundHeight + GetVerticalOffset() + Local
		: GetSurfaceHeight();
}

float AARPGSolidBody::GetSurfaceEnergyDensity() const
{
	return Definition ? Definition->EnergyPerArea : 0.f;
}

double AARPGSolidBody::GetMinimumArea() const
{
	return Definition ? Definition->MinimumArea : 0.0;
}

UMaterialInterface* AARPGSolidBody::ResolveSurfaceMaterial() const
{
	return Definition ? Definition->SurfaceMaterial.LoadSynchronous() : nullptr;
}

void AARPGSolidBody::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AARPGSolidBody, Definition);

	// The RESULT of the buoyancy settle, not its inputs. A client cannot see who
	// is standing on a floe accurately enough to arrive at the same number, and
	// four bytes is cheaper than trying.
	DOREPLIFETIME(AARPGSolidBody, Draft);

	// The hole matters on the client as much as the outline does: it is a gap you
	// can fall through, so a client that meshed and collided the slab without it
	// would let a player stand on air over the melted-out middle.
	// THE FIELD, which is the slab. Millimetre integers rather than floats for
	// exactly this reason -- see FARPGSolidField -- and even so it is the heaviest
	// thing this system puts on the wire, so a floe replicates at a modest rate
	// and dirty-region updates are the obvious next economy.
	DOREPLIFETIME(AARPGSolidBody, Field);

	// WHERE THE FIELD IS AND WHICH WAY ROUND. The cells are the same on every
	// machine; these two say where to put them. A client with the field and not
	// the frame would draw every floe in the level at the origin, unturned.
	DOREPLIFETIME(AARPGSolidBody, FieldOrigin);
	DOREPLIFETIME(AARPGSolidBody, FieldYaw);
}

void AARPGSolidBody::RebuildFromRing()
{
	// THE MESH IS CURRENT AS OF HERE, whatever asked for it -- a spell, a
	// refreeze, or the ambient melt that had been saving up. Cleared in the one
	// place that makes it true rather than at each caller, where it is one line
	// somebody adding a third melt path would not know to write.
	UndrawnMelt = 0.f;

	// THE TOTALS ARE TRANSIENT, so a client that has just received the field has
	// the cells and none of the sums. Rebuilding is a write-time cost and this is
	// only ever reached on a write -- the per-frame tick reads the cache and never
	// comes through here -- so paying for one sweep is what makes the cache safe
	// to trust everywhere else.
	Field.Refresh();

	if (!Definition || !Field.IsValidField() || Field.SolidCellCount() == 0)
	{
		ARPGFluidGeometry::BuildFieldMesh(Surface, FARPGSolidField(), FVector2D::ZeroVector,
			FARPGSurfaceFacets());
		Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	// FROM THE FIELD, not from an outline. Where the ice is, how thick it is and
	// how high it stands are all one answer now, and the box, the mesh and the
	// collision are three readings of it.
	const FVector2D Centre = Field.SolidCentroid();
	const FVector2D Reach(
		Field.SupportDistance(Centre, FVector2D(1, 0)),
		Field.SupportDistance(Centre, FVector2D(0, 1)));

	// THROUGH THE FRAME. The mesh is built in field space relative to this same
	// centroid, so the actor carrying the field's origin and yaw is what puts both
	// in the right place -- and the box extent stays in field space, because a
	// component's own bounds are local and turn with it.
	const FVector2D World = ToWorld(Centre);

	SetActorLocationAndRotation(
		FVector(World.X, World.Y, GroundHeight + GetVerticalOffset()),
		FRotator(0.f, FieldYaw, 0.f));

	Bounds->SetBoxExtent(FVector(
		FMath::Max(1.f, static_cast<float>(Reach.X)),
		FMath::Max(1.f, static_cast<float>(Reach.Y)),
		FMath::Max(1.f, Definition->Thickness * 4.f + 100.f)));

	Volume->SurfaceHeightOffset = Definition->Thickness;

	ARPGFluidGeometry::BuildFieldMesh(Surface, Field, Centre, Definition->Facets);

	// AND WHATEVER IS LYING ON IT. A spell that cuts the rock is usually the same
	// spell that leaves a film in the cut, and both arrive in the same write.
	RebuildFilm();

	if (UMaterialInterface* Material = ResolveSurfaceMaterial())
	{
		Surface->SetMaterial(0, Material);
	}

	// A slab is a thing you stand on and a body you can melt, but never an ambient
	// source and never a conductor -- see the notes on each.
	Volume->SetEnergy(GetArea() * GetSurfaceEnergyDensity());
	Volume->bReservoir = false;
	Volume->bAmbientSource = false;
	Volume->Conductivity = 0.f;

	if (!Definition->bStandable)
	{
		Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	// THE DRAWN SURFACE IS THE WALKABLE ONE, and now that the drawn surface has a
	// bowl melted into it and a hole through it, so does the collision. Nothing
	// approximates the other.
	Surface->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Surface->SetCollisionObjectType(ECC_WorldDynamic);
	Surface->SetCollisionResponseToAllChannels(ECR_Block);
	Surface->EnableComplexAsSimpleCollision();

	// COOKING IS THE EXPENSIVE PART, and it is worth exactly nothing for a slab
	// nobody can reach. A melting floe rebuilds four times a second, and cooking a
	// triangle mesh that often for every floe in the level is the one cost in this
	// system that scales with things the player will never touch. Deferred rather
	// than skipped: the flag stays set, so the first rebuild near a player cooks.
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
	if (!World || Field.SolidCellCount() == 0)
	{
		return 0;
	}

	// The reach is measured in the field's own frame, then the box that sweeps for
	// pawns is placed in the world. Generous either way: it is a broadphase, and
	// IsStandableAt is what actually decides.
	const FVector2D Local = Field.SolidCentroid();
	const FVector2D Extent(
		Field.SupportDistance(Local, FVector2D(1, 0)),
		Field.SupportDistance(Local, FVector2D(0, 1)));

	const FVector2D Centre = ToWorld(Local);
	const float Top = GetSurfaceHeight();

	// A shallow slice ABOVE the slab, because what is standing on it is not
	// overlapping it -- the mesh blocks, and a blocking contact generates no
	// overlap. Anything with feet in this slice is a candidate; the polygon test
	// below decides.
	static constexpr float StandingSlice = 120.f;

	TArray<FOverlapResult> Overlaps;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ARPGFloeOccupants), /*bTraceComplex=*/false);
	Params.AddIgnoredActor(this);

	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_Pawn);

	World->OverlapMultiByObjectType(Overlaps,
		FVector(Centre.X, Centre.Y, Top + StandingSlice * 0.5f),
		FQuat::Identity, ObjectParams,
		FCollisionShape::MakeBox(FVector(Extent.X, Extent.Y, StandingSlice * 0.5f)), Params);

	TSet<const AActor*> Counted;
	for (const FOverlapResult& Result : Overlaps)
	{
		const AActor* Actor = Result.GetActor();
		if (!Actor || Counted.Contains(Actor))
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

	const double Area = Field.SolidArea();
	if (Area <= 0.0 || FluidDensity <= 0.f)
	{
		return 0.f;
	}

	// ARCHIMEDES. A floating body displaces its own weight, so the depth it rides
	// at is mass over the fluid it has to push aside:
	//
	//     Draft = (SlabMass + LoadMass) / (FluidDensity * Area)
	//
	// The slab's own term reduces to AverageThickness * Density / FluidDensity,
	// with the area cancelling -- which is why a big slab and a small one of the
	// same stuff ride at the same depth, and correctly so. The load's term does
	// NOT cancel, so a wider slab takes a person's weight better. Both fall out of
	// the equation rather than being arranged.
	const float AverageThickness = static_cast<float>(Field.SolidVolume() / Area);
	const float SlabDraft = AverageThickness * (Definition->Density / FluidDensity);

	const float LoadMass = OccupantCount * Definition->OccupantMass * Definition->LoadResponse;
	const float LoadDraft = LoadMass / (FluidDensity * static_cast<float>(Area));

	// Never further than under. Past this the slab is swamped, and letting it keep
	// sinking would drag whoever is standing on it through the floor.
	return FMath::Min(SlabDraft + LoadDraft, AverageThickness);
}

bool AARPGSolidBody::HasRoomToward(const FVector2D& Direction) const
{
	if (!FloatsOn || Field.SolidCellCount() == 0)
	{
		return false;
	}

	// Clear water this far past the edge counts as room. Smaller than a floe and
	// larger than one cell of the grid it is measured on.
	static constexpr double Clearance = 50.0;

	const FVector2D Centre = Field.SolidCentroid();
	const double Reach = Field.SupportDistance(Centre, DirToField(Direction));

	// The probe goes back out into the world, because what it asks -- is there
	// still water over there -- is a question about the world and not the slab.
	return FloatsOn->IsSurfaceAt(ToWorld(Centre) + Direction * (Reach + Clearance));
}

void AARPGSolidBody::UpdateAnchoring()
{
	bAnchored = false;

	if (!FloatsOn || Field.SolidCellCount() == 0)
	{
		return;
	}

	// WEDGED, not merely touching. A floe pushed against one bank is still a raft
	// -- the current can turn it, slide it along, work it free. One that reaches
	// the shore on OPPOSITE sides is braced between them, which is what a spell
	// freezing the whole width of a river produces and what nothing should move.
	//
	// Four opposing pairs is enough to catch a plug across a river of any
	// orientation without pretending to more precision than a polygon has.
	static constexpr int32 Pairs = 4;

	for (int32 Index = 0; Index < Pairs; ++Index)
	{
		const double Angle = PI * Index / Pairs;
		const FVector2D Direction(FMath::Cos(Angle), FMath::Sin(Angle));

		if (!HasRoomToward(Direction) && !HasRoomToward(-Direction))
		{
			bAnchored = true;
			return;
		}
	}
}

void AARPGSolidBody::AdoptField(UARPGSolidDefinition* InDefinition,
	const FARPGSolidField& InField, const FVector& Where, float Yaw)
{
	Definition = InDefinition;
	Field = InField;
	GroundHeight = static_cast<float>(Where.Z);

	if (Definition && Definition->Element)
	{
		Volume->Element = Definition->Element;
	}

	// THE FRAME PLACES IT. The field's cells are wherever they were when it was
	// picked up, and moving those would be resampling a grid -- so the frame is
	// set so that the field's centroid lands on the point asked for.
	//
	// UPRIGHT, whatever it was doing in the air, and only yaw survives the flight.
	// A heightfield's columns are vertical by construction; landing one on its
	// side would mean resampling through an arbitrary rotation, which is lossy,
	// expensive, and exactly what makes tumbling hard for this shape. It slams
	// down flat, which is also the readable outcome.
	FieldYaw = Yaw;
	FieldOrigin = FVector2D(Where.X, Where.Y) - Field.SolidCentroid().GetRotated(Yaw);

	// The transient totals do not survive a copy any more than they survive the
	// wire, so the rebuild has to start by putting them back.
	Field.Refresh();
	RebuildFromRing();
}

void AARPGSolidBody::BeginBuried(float Depth)
{
	Draft = FMath::Max(0.f, Depth);

	// AND PUT IT THERE, rather than only recording that it should be. Setup has
	// already placed the slab at its resting height, so setting the number alone
	// would leave it standing in full view until the first tick moved it down --
	// a slab that appears and then sinks before rising, which is worse than not
	// animating at all.
	const FVector2D World = GetWorldCentre();
	SetActorLocation(FVector(World.X, World.Y, GroundHeight - Draft));
}

double AARPGSolidBody::DistanceToEdge(const FVector2D& World) const
{
	const FVector2D Local = ToField(World);
	const FVector2D Centre = Field.SolidCentroid();
	const FVector2D Toward = Local - Centre;

	if (Toward.IsNearlyZero())
	{
		return 0.0;
	}

	const double Support = Field.SupportDistance(Centre, Toward.GetSafeNormal());
	return FMath::Max(0.0, Toward.Size() - Support);
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
		Field.SupportDistance(Field.SolidCentroid(), DirToField(Across)));

	const FVector2D Left = FloatsOn->GetSurfaceFlowAt(Centre + Across * Half);
	const FVector2D Right = FloatsOn->GetSurfaceFlowAt(Centre - Across * Half);

	// Only the component ALONG the current differs in a way that turns you; a
	// difference across it is the flow converging, which shoves rather than spins.
	const double Shear = FVector2D::DotProduct(Left - Right, Along) / (2.0 * Half);

	// CRUDE ON PURPOSE: no angular momentum, no moment of inertia, no damping.
	// A floe that turns at all reads as an object; one that turns correctly reads
	// exactly the same and costs a solver nobody asked for.
	FieldYaw = FMath::UnwindDegrees(FieldYaw + static_cast<float>(
		FMath::RadiansToDegrees(Shear) * Definition->SpinResponse * DeltaTime));
}

void AARPGSolidBody::Rise(float DeltaTime)
{
	// DRAFT ALREADY MEANS THIS. For a floe it is how far under the waterline the
	// slab is riding; for a slab coming out of the ground it is how much of it is
	// still buried. One number, and the actor is already placed at
	// GroundHeight - Draft, so pulling it to zero IS the rise with nothing else
	// to write.
	if (FMath::IsNearlyZero(Draft))
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

	const FVector2D World = GetWorldCentre();
	SetActorLocation(FVector(World.X, World.Y, GroundHeight - Draft));
}

void AARPGSolidBody::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Server only. The draft replicates as a result and the outline carries the
	// drift, so a client that simulated its own would be fighting both.
	// IsValid rather than a null check on FloatsOn: an actor destroyed this frame
	// is not garbage collected until later, so the interface still points at it
	// and every query below would run against a dead pool.
	if (!HasAuthority() || !Definition || Field.SolidCellCount() == 0)
	{
		return;
	}

	// WHEREVER THE SLAB IS. Water running off a floe is the same water running off
	// a tower, so this sits above the split rather than in either half -- and it
	// costs one cached comparison on a slab nobody has melted.
	TickRunoff(DeltaTime);

	// ROOTED, so there is nothing to float on and nothing to work out. A slab
	// raised out of the ground has no FloatsOn: it does not settle, it does not
	// drift, and every query below would be asked of a surface that is not there.
	// All it has is a rise, and once that is finished it never moves again.
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
	// for someone watching or standing on it: a floe two hundred metres away that
	// bobs and slides is a physics query and a transform update per frame, per
	// floe, for something nobody can perceive. It keeps MELTING, because that
	// happens on the weather tick and is the state that has to stay honest.
	if (const UARPGFluidSurfaceSubsystem* Fluids =
			GetWorld() ? GetWorld()->GetSubsystem<UARPGFluidSurfaceSubsystem>() : nullptr)
	{
		if (!Fluids->IsSignificantAt(GetActorLocation()))
		{
			Surface->ComponentVelocity = FVector::ZeroVector;
			return;
		}
	}

	// IN THE WORLD, because everything below asks the surface underneath about a
	// place -- a waterline, a bed, a current -- and the surface has never heard of
	// this slab's frame.
	const FVector2D Centre = GetWorldCentre();

	// The waterline it should be riding, asked of the body it froze out of -- so a
	// floe on a Water plugin river follows the waves and one on a puddle sits on a
	// flat number, without this knowing which it is on.
	GroundHeight = FloatsOn->GetSurfaceLevelAt(Centre);

	// FOUR TIMES A SECOND, not sixty. This is a physics overlap, and it is the last
	// per-frame scan left in the system after the culling pass -- while what it
	// feeds is a draft that settles over about a third of a second, so a load
	// arriving up to 250ms late is inside the lag the settle already has.
	OccupantPoll -= DeltaTime;

	if (OccupantPoll <= 0.f)
	{
		OccupantPoll = OccupantPollInterval;
		OccupantCount = CountOccupants();
	}

	// TOO HEAVY TO FLOAT IS NOT A FAILURE. A slab denser than the fluid it formed
	// out of sinks and comes to rest on the bed, which is one comparison on the
	// same equation -- and it is why nothing in here is named for ice or water. A
	// crust of obsidian on lava settles by exactly this line.
	bAground = Definition->Density >= FloatsOn->GetSurfaceDensity();

	const float TargetDraft = bAground
		? GroundHeight - FloatsOn->GetSurfaceBedAt(Centre)
		: ComputeTargetDraft();

	// SETTLE toward it rather than snapping. A step change would teleport anyone
	// standing on the slab, and the lag is most of what makes a surface feel like
	// it gives under a footfall rather than being a lift.
	Draft = FMath::FInterpTo(Draft, TargetDraft, DeltaTime, Definition->SettleSpeed);

	SetActorLocation(FVector(Centre.X, Centre.Y, GroundHeight - Draft));

	if (bAground)
	{
		// Sitting on the bottom. The current has nothing to lift it with.
		Surface->ComponentVelocity = FVector::ZeroVector;
		return;
	}

	if (bAnchored)
	{
		// Braced on both banks. Still bobs -- the water under it is still there --
		// but the current has nothing to push against.
		Surface->ComponentVelocity = FVector::ZeroVector;
		return;
	}

	const FVector2D Flow = FloatsOn->GetSurfaceFlowAt(Centre) * Definition->DriftResponse;
	const FVector2D Step = Flow * DeltaTime;

	// Not off the end of its own water. The march is one containment probe, and
	// refusing the step is what stops a floe beaching itself on a bank the anchor
	// test did not catch because it only reaches the shore on one side.
	if (!Step.IsNearlyZero() && HasRoomToward(Step.GetSafeNormal()))
	{
		// THE FRAME SLIDES, not the field and certainly not its cells. Drifting is
		// one vector add on the origin -- cheaper even than translating the field
		// was, because the field's cached centroid does not have to move with it.
		FieldOrigin += Step;
		SetActorLocation(GetActorLocation() + FVector(Step.X, Step.Y, 0.f));

		// AND IT TURNS. A floe that slides down a river without ever spinning is
		// the tell that it is a grid rather than an object, and the river already
		// knows enough to fix it: sample the current at both flanks and the
		// difference across the slab IS the shear that turns it. Crude -- one
		// sample pair, no angular momentum -- but it is the difference between ice
		// that drifts and ice that behaves.
		Spin(DeltaTime, Centre, Flow);

		// SO IT CARRIES THE PLAYER. A character standing on a kinematic base is
		// moved by UCharacterMovementComponent's based movement, and the base's
		// velocity is what it imparts when they jump off. Without this the ice
		// slides out from under them, which is worse than not drifting at all.
		Surface->ComponentVelocity = FVector(Flow.X, Flow.Y, 0.f);
	}
	else
	{
		Surface->ComponentVelocity = FVector::ZeroVector;
	}
}

bool AARPGSolidBody::IsPermanent() const
{
	// THE SAME ZERO that makes the weather tick skip it. MeltRate is "does time
	// take this", so a slab that answers no to time should not be taken by a
	// budget either -- obsidian, and a wall of earth raised out of the ground.
	return Definition && FMath::IsNearlyZero(Definition->MeltRate);
}

double AARPGSolidBody::GetArea() const
{
	// The ice that is actually LEFT, which after a fireball is not the outline it
	// froze with. Energy, buoyancy and retirement all read this.
	return Field.SolidArea();
}

double AARPGSolidBody::MeltAt(const FVector2D& Where, float Radius, float Depth)
{
	// A world contact, asked of the field in its own frame.
	const double Removed = Field.MeltBowl(ToField(Where), Radius, Depth);

	if (Removed > 0.0)
	{
		RebuildFromRing();
		UpdateAnchoring();
	}

	return Removed;
}

double AARPGSolidBody::MeltUniformly(float FromTop, float FromBottom)
{
	// THE SHAPE IS WHAT HAS TO BE DRAWN PROMPTLY, not the thickness. A cell that
	// has stopped being solid is a hole opening or a rim retreating -- something
	// with an outline, which the player sees the instant it happens. A slab that
	// is a millimetre thinner everywhere is not, and asking for a full rebuild and
	// a collision cook for one is what made a melting floe cost a frame.
	const int32 Before = Field.SolidCellCount();

	const double Removed = Field.MeltUniform(FromTop, FromBottom);

	if (Removed <= 0.0)
	{
		return 0.0;
	}

	UndrawnMelt += FMath::Max(0.f, FromTop) + FMath::Max(0.f, FromBottom);

	if (Field.SolidCellCount() != Before || UndrawnMelt >= UndrawnMeltLimit)
	{
		RebuildFromRing();
	}

	return Removed;
}

double AARPGSolidBody::MeltInward(float Distance)
{
	const double Removed = Field.Erode(Distance);

	// ALWAYS DRAWN, unlike a thinning. Erosion only ever removes whole cells, so
	// anything it did at all is a change to the silhouette.
	if (Removed > 0.0)
	{
		RebuildFromRing();
	}

	return Removed;
}

void AARPGSolidBody::GroundOnBed(float BedHeight)
{
	FloatsOn = nullptr;
	bAground = true;

	// DRAFT IS HOW FAR UNDER ITS RESTING HEIGHT THE SLAB IS SITTING, and a slab
	// on the floor is at its resting height by definition. Cleared rather than
	// left, or Rise would spend the next second pulling a slab that is already
	// home the rest of the way there.
	Draft = 0.f;
	GroundHeight = BedHeight;

	const FVector2D World = GetWorldCentre();
	SetActorLocation(FVector(World.X, World.Y, GroundHeight));
}

bool AARPGSolidBody::ConsumeSurfaceArea(double Area)
{
	// AN AREA IS NOT WHAT HAPPENS TO A SLAB. A pool loses ground uniformly because
	// a liquid has no third dimension to lose it in; ice melts WHERE it was hit.
	// The reaction path calls MeltAt with the contact instead, and this remains
	// only for anything that still asks in area -- thinning the whole slab by the
	// depth that much ice would have been.
	const double Plan = Field.SolidArea();
	if (Plan <= 0.0 || !Definition)
	{
		return true;
	}

	// WHERE IT WAS HIT, when the solver told us. The volume of ice a reaction is
	// worth becomes a bowl at the contact: deep in the middle, tapering out, so a
	// hit at the edge cuts the slab away at an angle and one in the middle opens a
	// hole through it. Without a contact -- anything that spent this slab without
	// touching a point on it -- fall back to thinning the whole thing.
	double Melted = 0.0;
	FVector2D MeltedAt = GetWorldCentre();

	if (bHasPendingContact)
	{
		bHasPendingContact = false;
		MeltedAt = PendingContact;

		// A bowl of this radius and depth removes about half a cylinder's volume,
		// so the depth that spends the given plan-area of ice is twice as deep as
		// a flat cut would be.
		const float Radius = FMath::Max(Definition->CellSize, Definition->MeltRadius);
		const float Depth = static_cast<float>(2.0 * Area / (PI * Radius * Radius))
			* Definition->Thickness;

		Melted = MeltAt(PendingContact, Radius, Depth);
	}
	else
	{
		Melted = MeltUniformly(static_cast<float>(Area / Plan), 0.f);
	}

	// THE ONLY WAY A SOLID GIVES ITS FLUID BACK. Ambient melting deliberately
	// returns nothing -- a floe left alone in the sun thins away and the world is
	// no wetter for it, because a puddle for every floe that ever existed is
	// bookkeeping nobody asked to see. A reaction is the opposite case: a fireball
	// through ice is a thing the player did, at a place they can see, and the
	// water it leaves is the visible result of it.
	ReturnMeltedFluid(Melted, MeltedAt);

	return Field.SolidCellCount() == 0 || Field.SolidArea() < GetMinimumArea();
}

void AARPGSolidBody::ReturnMeltedFluid(double MeltedVolume, const FVector2D& At)
{
	// Null MeltsInto is correct for obsidian: rock that formed on lava is not
	// frozen lava, and breaking it releases nothing.
	if (MeltedVolume <= 0.0 || !Definition || !Definition->MeltsInto)
	{
		return;
	}

	UARPGFluidDefinition* Fluid = Definition->MeltsInto;

	UARPGFluidSurfaceSubsystem* Fluids =
		GetWorld() ? GetWorld()->GetSubsystem<UARPGFluidSurfaceSubsystem>() : nullptr;

	// SAID FIRST, AND SAID WHATEVER HAPPENS NEXT. The reaction's own product would
	// otherwise deposit this same fluid a second time when its discharge lands --
	// for fire + ice -> water those are one body of water described twice.
	//
	// Above every early return below on purpose. Absorbed into a lake counts as
	// accounted for, and so does running down a tower: that the water has not
	// arrived yet does not mean nobody is bringing it.
	if (Fluids)
	{
		Fluids->NoteFluidReturned(Fluid->GetElementTag());
	}

	// MASS IS WHAT IS CONSERVED, not volume. Ice is lighter than the water it came
	// from, so a cubic metre of it does not melt into a cubic metre -- it melts
	// into the volume of water that weighs the same. The same two densities that
	// decide whether the slab floats decide how much water it is worth, which is
	// the point of them being densities rather than a float called Buoyancy.
	double FluidVolume = MeltedVolume
		* Definition->Density / FMath::Max(KINDA_SMALL_NUMBER, Fluid->Density);

	// ONTO THE SLAB FIRST, so the water has to get down before it is anywhere.
	//
	// THIS IS THE JOURNEY THAT USED TO BE MISSING. Melting the top of a tower put
	// a puddle at its foot in the same instant -- the right destination reached by
	// no route at all. Poured at the bowl the fireball cut, the film runs down the
	// slab's own heightfield and arrives over a second or two, on the side that
	// was hit. Pour hands back only what found nowhere to land -- through a hole
	// melted clean through, or off a slab too small to have a grid -- and that
	// carries on to the ground exactly as all of it used to.
	if (Fluid->FlowRate > 0.f)
	{
		FluidVolume = Field.Pour(ToField(At), Definition->MeltRadius, FluidVolume);

		// DRAWN THE INSTANT IT IS POURED, not on the next tick. The melt above
		// already rewrote the slab's mesh -- before this line, while the field was
		// still dry -- so leaving the film to the runoff tick meant the frame the
		// fireball landed on showed a fresh crater with nothing in it.
		RebuildFilm();

		if (FluidVolume <= 0.0)
		{
			// ALL OF IT LANDED ON THE SLAB, which is what a bowl cut into the
			// middle does with its own melt. There is no pool coming and never
			// will be, so the film just drawn is the entire visible result of the
			// spell -- see Slabs.MeltedLavaIsDrawnOnTheSlab.
			return;
		}
	}

	// BACK INTO WHATEVER IT IS FLOATING ON, and the slab never learns which kind
	// of thing that is: a lake takes it and nothing appears, a puddle takes it by
	// growing its outline. Only a slab left on dry land -- its pool evaporated out
	// from under it -- falls through to making a body of its own.
	//
	// IsValid rather than a null check: a pool destroyed this frame has not been
	// garbage collected yet, so the interface still points at it.
	IARPGElementalSurface* Riding =
		IsValid(FloatsOn.GetObject()) ? FloatsOn.GetInterface() : nullptr;

	if (Riding && Riding->IsSurfaceAt(At) && Riding->AbsorbSurfaceVolume(FluidVolume))
	{
		return;
	}

	if (!Fluids)
	{
		return;
	}

	// THE BED, not the waterline. GroundHeight on a slab tracks the surface it
	// rides, which is where the slab is -- but fluid lies on the bottom. For a floe
	// melting on a puddle those are centimetres apart and either would do; for one
	// melting on dry land after its pool evaporated, the bed is the only answer
	// that is not in the air.
	const float Bed = Riding ? Riding->GetSurfaceBedAt(At) : GroundHeight;

	Fluids->ReturnFluid(At, Bed, FluidVolume, Fluid);
}

float AARPGSolidBody::GroundBelow() const
{
	// A ROOTED SLAB STANDS ON ITS OWN BASE, and a floating one on the bed of
	// whatever carries it. Both are already known, so a trace would be paying for
	// an answer the body is holding.
	const IARPGElementalSurface* Riding =
		IsValid(FloatsOn.GetObject()) ? FloatsOn.GetInterface() : nullptr;

	return Riding ? Riding->GetSurfaceBedAt(GetWorldCentre()) : GroundHeight;
}

double AARPGSolidBody::GetWallVolume() const
{
	double Held = 0.0;
	for (const FARPGWallRun& Run : WallRuns)
	{
		Held += Run.Volume;
	}
	return Held;
}

double AARPGSolidBody::GetPendingRunoff() const
{
	return PendingRunoff + GetWallVolume();
}

void AARPGSolidBody::ShedOntoTheWall(const FVector2D& ShedAt, double Shed)
{
	if (Shed <= 0.0 || !Field.IsValidField())
	{
		return;
	}

	// ACROSS EVERY WET RIM CELL, in proportion to how wet each one is.
	//
	// NOT AT ShedAt, WHICH IS AN AVERAGE. The flow solver reports one position for
	// a whole tick's shedding, weighted across everywhere it happened -- and for a
	// tower draining evenly all the way round, the average of every point on the
	// rim is the MIDDLE of the slab. There is no face there to run down, so
	// placing the run by that number put the entire runoff nowhere. It is the
	// right answer for "where should the puddle form" and the wrong one for
	// "which side did it go over".
	static const FIntPoint Steps[4] = { {0, -1}, {1, 0}, {0, 1}, {-1, 0} };

	struct FSpill
	{
		FIntPoint Cell;
		FIntPoint Side;
		float Foot;
		double Weight;
	};

	TArray<FSpill> Spills;
	double Total = 0.0;

	for (int32 Y = 0; Y < Field.CountY; ++Y)
	{
		for (int32 X = 0; X < Field.CountX; ++X)
		{
			if (!Field.IsSolid(X, Y))
			{
				continue;
			}

			const int32 At = Field.Index(X, Y);
			const double Depth = Field.Wet.IsValidIndex(At) ? Field.Wet[At] : 0.0;

			if (Depth <= 0.0)
			{
				continue;
			}

			const double RockTop = Field.Top[At] * 0.1;

			// THE STEEPEST WAY OFF. Where several sides are open the fluid takes
			// the biggest drop, which is the outside of the slab rather than a
			// terrace one step down.
			double Deepest = 0.0;
			FIntPoint Best = FIntPoint::ZeroValue;

			for (const FIntPoint& Step : Steps)
			{
				const int32 NX = X + Step.X;
				const int32 NY = Y + Step.Y;

				const bool bInside = NX >= 0 && NX < Field.CountX
					&& NY >= 0 && NY < Field.CountY;
				const bool bNeighbour = bInside && Field.IsSolid(NX, NY);

				const double Drop = bNeighbour
					? RockTop - Field.Top[Field.Index(NX, NY)] * 0.1
					: RockTop - Field.Bottom[At] * 0.1;

				if (Drop > Deepest)
				{
					Deepest = Drop;
					Best = Step;
				}
			}

			if (Best == FIntPoint::ZeroValue)
			{
				continue; // an interior cell, with nowhere to spill
			}

			Spills.Add({ FIntPoint(X, Y), Best,
				static_cast<float>(RockTop - Deepest), Depth });
			Total += Depth;
		}
	}

	if (Spills.Num() == 0 || Total <= 0.0)
	{
		// NOTHING WET AT AN EDGE. The solver shed from somewhere with no way off,
		// which should not happen -- but handing the volume to the batch is the
		// answer that cannot lose it.
		PendingRunoff += Shed;
		return;
	}

	for (const FSpill& Spill : Spills)
	{
		const double Share = Shed * (Spill.Weight / Total);
		const int32 At = Field.Index(Spill.Cell.X, Spill.Cell.Y);
		const FVector2D Where = Field.CentreOf(Spill.Cell.X, Spill.Cell.Y);

		// ONE STREAM PER PLACE IT POURS FROM, fed for as long as it keeps
		// pouring. A fresh run per tick would be a new rivulet every frame --
		// hundreds of them, one cell-height apart -- where what is actually there
		// is one stream running continuously.
		FARPGWallRun* Existing = WallRuns.FindByPredicate(
			[&](const FARPGWallRun& Run)
			{
				return Run.Side == Spill.Side && Run.At.Equals(Where, 1.0);
			});

		if (Existing)
		{
			Existing->Volume += Share;
			continue;
		}

		FARPGWallRun Run;
		Run.At = Where;
		Run.Side = Spill.Side;
		Run.Volume = Share;
		Run.Front = static_cast<float>(Field.Top[At] * 0.1);
		Run.Foot = Spill.Foot;
		Run.Thickness = FMath::Max(1.f, static_cast<float>(Spill.Weight));

		WallRuns.Add(Run);
	}
}

void AARPGSolidBody::TickWallRuns(float DeltaTime, const UARPGFluidDefinition& Fluid)
{
	if (WallRuns.Num() == 0)
	{
		return;
	}

	// THE VISCOSITY, AS A SPEED. See the definition: a wall is vertical
	// everywhere, so there is no slope for a settling rate to read and the only
	// thing separating water from lava is how fast it moves down the face.
	const float Speed = FMath::Max(0.1f, Fluid.WallSpeed);

	for (int32 Index = WallRuns.Num() - 1; Index >= 0; --Index)
	{
		FARPGWallRun& Run = WallRuns[Index];

		Run.Front = FMath::Max(Run.Foot, Run.Front - Speed * DeltaTime);

		if (!Run.HasArrived())
		{
			continue;
		}

		// ARRIVED, so what it is carrying is on the ground. Into the batch rather
		// than deposited on the spot: every deposit is a polygon merge or an actor
		// spawn, and a stream running for seconds would be a boolean op a frame.
		// Where it came down is carried with it, so the pool forms under the side
		// it actually ran down.
		if (Run.Volume > 0.0)
		{
			const FVector2D LeftAt = ToWorld(Run.At);

			RunoffAt = PendingRunoff > 0.0
				? (RunoffAt * PendingRunoff + LeftAt * Run.Volume) / (PendingRunoff + Run.Volume)
				: LeftAt;

			PendingRunoff += Run.Volume;
			Run.Volume = 0.0;
		}

		// KEPT WHILE THE SOURCE IS STILL POURING. A stream that reached the bottom
		// is still a stream: it goes on delivering as long as the cell feeding it
		// is wet, and only stops being drawn once that has dried. Removing it on
		// arrival would restart the descent from the top every time another
		// trickle went over, which is a wall that flickers rather than one that
		// runs.
		const FIntPoint Cell = Field.CellAt(Run.At);
		const bool bSourceDry = Cell.X < 0
			|| !Field.Wet.IsValidIndex(Field.Index(Cell.X, Cell.Y))
			|| Field.Wet[Field.Index(Cell.X, Cell.Y)] <= Fluid.MinimumFilm;

		if (bSourceDry)
		{
			WallRuns.RemoveAtSwap(Index);
		}
	}
}

void AARPGSolidBody::TickRunoff(float DeltaTime)
{
	// FREE WHEN DRY, which is nearly always. HasWet is a cached total rather than
	// a sweep, so a slab nobody has melted pays one comparison per tick and an
	// earth wall pays that for the whole level.
	//
	// UNLESS SOMETHING IS STILL IN THE AIR. The last of a film dries on the same
	// tick it flushes, and that flush is usually still falling when it happens --
	// so returning on HasWet alone stranded the final batch a couple of metres
	// above the ground for the rest of the level. Pending runoff is zero whenever
	// a slab is genuinely idle, so the fast path costs the same comparison.
	// UNLESS SOMETHING IS STILL ON THE WALL, which is most of the time now that
	// leaving the rim is the START of the journey rather than the end of it.
	if (!Definition || (!Field.HasWet() && PendingRunoff <= 0.0 && WallRuns.Num() == 0))
	{
		return;
	}

	// HOW IT FLOWS IS THE FLUID'S BUSINESS, not the slab's. Water off ice and lava
	// off earth are the same solver told two different sets of numbers, and the
	// slab reads them through its own MeltsInto rather than carrying a copy.
	const UARPGFluidDefinition* Fluid = Definition->MeltsInto;
	if (!Fluid)
	{
		return; // nothing it could be wet with
	}

	FVector2D ShedAt = FVector2D::ZeroVector;
	const double Shed = Field.FlowStep(DeltaTime, Fluid->FlowRate, Fluid->YieldSlope,
		Fluid->MinimumFilm, ShedAt);

	// REDRAWN WHERE IT MOVED, which is the whole of what a film looks like: this
	// is reached only while a slab is actually draining, and a drain lasts
	// seconds. A slab nobody has melted never gets here at all -- see the HasWet
	// guard at the top -- so the cost is paid by exactly the thing being watched.
	//
	// AFTER the runs advance below as well as after the flow above, because a run
	// creeping down the face is a moving thing even when the top has dried.

	if (Shed > 0.0)
	{
		// ONTO THE FACE, not into the batch. What leaves the rim has not arrived
		// anywhere -- it is on the outside of the slab, working its way down at
		// whatever speed the stuff moves. Weighting by where it left still
		// happens, but on arrival rather than departure.
		ShedOntoTheWall(ShedAt, Shed);
	}

	TickWallRuns(DeltaTime, *Fluid);

	// Both the film on top and the sheets on the face have just moved.
	RebuildFilm();

	// BATCHED. Runoff arrives in dribbles by design and every deposit is a polygon
	// merge or an actor spawn, so putting each one down as it comes would charge a
	// melting tower a boolean op every frame for a teaspoon of water.
	//
	// FLUSHED EARLY when the film has finished, whatever is held: the last of it
	// is always under the batch, and water that never arrives because it was the
	// remainder is the kind of loss nobody can see happening but everybody
	// eventually notices.
	// NOT MERELY DRY ON TOP: a slab whose film has all gone over the edge still
	// has it, on the way down. Flushing then would put the last of the lava on
	// the ground before it had finished running there.
	const bool bFinished = !Field.HasWet() && WallRuns.Num() == 0;

	if (PendingRunoff <= 0.0 || (!bFinished && PendingRunoff < Fluid->RunoffBatch))
	{
		return;
	}

	// NO WAIT HERE ANY MORE. Getting down the outside of the slab used to be a
	// timed fall applied to the batch -- the time a stone would take, which for a
	// chest-high wall is half a second and reads as instant. The journey is now
	// the runs above, which creep at the fluid's own speed and only add to this
	// batch once they have actually arrived. What is in PendingRunoff is on the
	// ground already.

	if (UARPGFluidSurfaceSubsystem* Fluids =
			GetWorld() ? GetWorld()->GetSubsystem<UARPGFluidSurfaceSubsystem>() : nullptr)
	{
		// ON THE GROUND UNDER WHERE IT RAN OFF, which for a rooted tower is the
		// slab's own base and for a floe is the bed of whatever it floats in. The
		// slab is not ground -- the deposit path already knows to trace past every
		// body this subsystem owns.
		IARPGElementalSurface* Riding =
			IsValid(FloatsOn.GetObject()) ? FloatsOn.GetInterface() : nullptr;

		const float Bed = Riding ? Riding->GetSurfaceBedAt(RunoffAt) : GroundHeight;

		Fluids->ReturnFluid(RunoffAt, Bed, PendingRunoff, Definition->MeltsInto);
	}

	PendingRunoff = 0.0;
}

bool AARPGSolidBody::ContainsPoint(FVector WorldPoint) const
{
	return Field.IsSolidAt(ToField(FVector2D(WorldPoint.X, WorldPoint.Y)));
}

bool AARPGSolidBody::IsStandableAt(FVector WorldPoint) const
{
	if (!Definition || !Definition->bStandable)
	{
		return false;
	}

	// ONE QUESTION NOW. A hole used to be a second ring the outline had to be
	// checked against separately, and keeping the two in step was most of the
	// complexity; here it is simply a cell whose top has met its bottom, so "is
	// there ice here" is the entire test and a gap you can fall through needs no
	// special knowledge at all.
	return Field.IsSolidAt(ToField(FVector2D(WorldPoint.X, WorldPoint.Y)));
}
