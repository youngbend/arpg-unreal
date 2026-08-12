// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGFluidBody.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGFluidDefinition.h"
#include "ARPGFluidGeometry.h"
#include "ARPGSolidField.h"
#include "ARPGFluidSurfaceSubsystem.h"
#include "ARPGMagicElement.h"
#include "Components/BoxComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"

FGameplayTag UARPGFluidDefinition::GetElementTag() const
{
	return Element ? Element->ElementTag : FGameplayTag();
}

FGameplayTag UARPGSolidDefinition::GetElementTag() const
{
	return Element ? Element->ElementTag : FGameplayTag();
}

AARPGFluidBody::AARPGFluidBody()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;

	Bounds = CreateDefaultSubobject<UBoxComponent>(TEXT("Bounds"));
	Bounds->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Bounds->SetCollisionObjectType(ECC_WorldDynamic);
	Bounds->SetCollisionResponseToAllChannels(ECR_Overlap);
	Bounds->SetGenerateOverlapEvents(true);

	// Movable, or the engine never updates its overlaps and nothing entering the
	// body is noticed -- see UARPGElementalVolumeComponent's own warning.
	Bounds->SetMobility(EComponentMobility::Movable);
	SetRootComponent(Bounds);

	Volume = CreateDefaultSubobject<UARPGElementalVolumeComponent>(TEXT("Volume"));
	Volume->SetupAttachment(Bounds);
	Volume->OverlapSource = Bounds;
	Volume->bAmbientSource = true;

	Surface = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("Surface"));
	Surface->SetupAttachment(Bounds);
	Surface->SetMobility(EComponentMobility::Movable);

	// NO COLLISION BY DEFAULT, because water is not a thing you stand on. A solid
	// turns this on for itself, which is the one case where the drawn surface and
	// the walkable one have to be the same surface.
	Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// A PUDDLE IS NOT A LANDMARK. Frustum culling comes free from the component's
	// own bounds, but nothing stops fifty of them being submitted from across a
	// valley -- and at that range a body is a few pixels of tinted ground. The
	// engine fades it out instead.
	Surface->SetCullDistance(15000.f);
}

void AARPGFluidBody::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// THE OUTLINE, NOT THE MESH. Everything a client draws and walks on is a pure
	// function of these two, so a floe costs a few dozen FVector2Ds rather than
	// vertex buffers, and server and client geometry are identical by construction
	// instead of by hoping the cook matched.
	DOREPLIFETIME(AARPGFluidBody, Ring);
	DOREPLIFETIME(AARPGFluidBody, GroundHeight);
}

void AARPGFluidBody::OnRep_Body()
{
	RebuildFromRing();
}

const TArray<FVector2D>& AARPGFluidBody::GetMeshHole() const
{
	static const TArray<FVector2D> None;
	return None;
}

TArray<FVector2D> AARPGFluidBody::GetSurfaceFootprint(const FVector2D& Centre, double Radius) const
{
	return Ring;
}

float AARPGFluidBody::GetSurfaceLevelAt(const FVector2D& At) const
{
	return GetSurfaceHeight();
}

bool AARPGFluidBody::IsSurfaceAt(const FVector2D& At) const
{
	return ARPGFluidGeometry::PolygonContains(Ring, At);
}

void AARPGFluidBody::TranslateRing(const FVector2D& Delta)
{
	if (Ring.Num() < 3 || Delta.IsNearlyZero())
	{
		return;
	}

	for (FVector2D& Point : Ring)
	{
		Point += Delta;
	}

	// The actor moves and NOTHING ELSE DOES. The mesh is local and a translation
	// leaves it identical; the trigger box keeps its extent; the volume keeps its
	// energy, because none of them are functions of WHERE the body is.
	const FVector Location = GetActorLocation();
	SetActorLocation(FVector(Location.X + Delta.X, Location.Y + Delta.Y, Location.Z));
}

bool AARPGFluidBody::ConsumeSurfaceArea(double Area)
{
	if (Area <= 0.0)
	{
		return false;
	}

	const double Remaining = FMath::Max(0.0, GetArea() - Area);

	if (Remaining < GetMinimumArea())
	{
		return true;
	}

	// The body keeps its SHAPE and loses the area, rather than having the region
	// cut out of it: a liquid flows back over a hole, and a slab that lost its
	// middle to a fireball would be a ring of ice standing on nothing.
	SetRing(ARPGFluidGeometry::ShrinkToArea(Ring, Remaining));
	return false;
}

void AARPGFluidBody::OnElementalReaction_Implementation(float Consumed, float Remaining,
	UARPGMagicElement* Product)
{
	const float Density = GetSurfaceEnergyDensity();

	// Amplified rather than spent, or a body nothing can eat -- a river, or one
	// whose definition never gave it a density. Either way there is no ground to
	// take, and saying so here is what keeps the default projectile reaction
	// (scale the actor, destroy it at zero) away from a body of fluid.
	if (Consumed <= 0.f || Density <= 0.f)
	{
		return;
	}

	// THE SPEND, CONVERTED BACK INTO GROUND. Energy is area times density, so the
	// inverse is the honest amount boiled or melted away -- which means the
	// combination table's consumption rates already decide how fast a fireball
	// eats a puddle, with no second set of numbers to keep in step.
	if (ConsumeSurfaceArea(Consumed / Density))
	{
		if (UARPGFluidSurfaceSubsystem* Fluids =
				GetWorld() ? GetWorld()->GetSubsystem<UARPGFluidSurfaceSubsystem>() : nullptr)
		{
			Fluids->RetireBody(this);
		}
	}
}

int32 AARPGFluidBody::GetSurfaceTriangleCount() const
{
	if (!Surface)
	{
		return 0;
	}

	int32 Count = 0;
	Surface->ProcessMesh([&Count](const UE::Geometry::FDynamicMesh3& Mesh)
	{
		Count = Mesh.TriangleCount();
	});

	return Count;
}

UPrimitiveComponent* AARPGFluidBody::GetSurfaceComponent() const
{
	return Surface;
}

UMaterialInterface* AARPGFluidBody::GetSurfaceMaterial() const
{
	return Surface ? Surface->GetMaterial(0) : nullptr;
}

void AARPGFluidBody::SetRing(const TArray<FVector2D>& NewRing)
{
	Ring = NewRing;
	RebuildFromRing();
}

double AARPGFluidBody::GetArea() const
{
	return ARPGFluidGeometry::PolygonArea(Ring);
}

bool AARPGFluidBody::ContainsPoint(FVector WorldPoint) const
{
	// The POLYGON, not the box. The box is broadphase; this is the truth.
	return ARPGFluidGeometry::PolygonContains(Ring, FVector2D(WorldPoint.X, WorldPoint.Y));
}

void AARPGFluidBody::RebuildFromRing()
{
	if (Ring.Num() < 3)
	{
		// NOT a bare return, now that there is something drawn. A body with no
		// outline left has to stop being visible: the subsystem destroys an eroded
		// pool, but a client can see the emptied ring replicate before the
		// destruction reaches it, and the difference between the two is a puddle
		// hanging in the air until the actor finally goes.
		ARPGFluidGeometry::BuildSlabMesh(Surface, Ring, GetMeshHole(),
			FVector2D::ZeroVector, /*BottomZ=*/0.f, /*TopZ=*/0.f);
		return;
	}

	const FBox2D Box = ARPGFluidGeometry::PolygonBounds(Ring);
	const FVector2D Centre = Box.GetCenter();
	const FVector2D Extent = Box.GetExtent();

	const float SurfaceOffset = GetSurfaceOffset();

	SetActorLocation(FVector(Centre.X, Centre.Y, GroundHeight + GetVerticalOffset()));

	// Generous headroom above the surface, so a spell arriving from above enters
	// the broadphase well before it reaches the waterline -- which is what lets
	// the reaction subsystem wait until it has actually arrived.
	Bounds->SetBoxExtent(FVector(
		FMath::Max(1.f, Extent.X),
		FMath::Max(1.f, Extent.Y),
		FMath::Max(1.f, SurfaceOffset * 4.f + 100.f)));

	Volume->SurfaceHeightOffset = SurfaceOffset;

	// THE SAME RING that decides everything else decides what you see, so the
	// drawn shape cannot drift from the simulated one -- there is only one shape.
	// Local space, because the actor sits at the centroid at ground height.
	ARPGFluidGeometry::BuildSlabMesh(Surface, Ring, GetMeshHole(), Centre,
		/*BottomZ=*/0.f, /*TopZ=*/SurfaceOffset);

	// Re-applied on every rebuild rather than once at setup: on a client the
	// definition arrives by replication and may land after the first ring, so
	// there is no single moment that is reliably "after we know what this is".
	if (UMaterialInterface* Material = ResolveSurfaceMaterial())
	{
		Surface->SetMaterial(0, Material);
	}
}

// ---------------------------------------------------------------------------
// Pool
// ---------------------------------------------------------------------------

void AARPGFluidPool::Setup(UARPGFluidDefinition* InDefinition, const TArray<FVector2D>& InRing,
	float InGroundHeight)
{
	Definition = InDefinition;
	GroundHeight = InGroundHeight;

	if (Definition && Definition->Element)
	{
		Volume->Element = Definition->Element;
	}

	SetRing(InRing);
}

float AARPGFluidPool::GetSurfaceOffset() const
{
	return Definition ? Definition->Depth : 0.f;
}

UMaterialInterface* AARPGFluidPool::ResolveSurfaceMaterial() const
{
	return Definition ? Definition->SurfaceMaterial.LoadSynchronous() : nullptr;
}

void AARPGFluidPool::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// The definition is an ASSET, so this replicates as a stable path rather than
	// as an object -- which is what lets a client size, colour and texture the
	// body from the same numbers the server used.
	DOREPLIFETIME(AARPGFluidPool, Definition);
}

bool AARPGFluidPool::ConsumeSurfaceArea(double Area)
{
	// A POOL BIG ENOUGH TO BE A RESERVOIR IS NOT DEPLETED, by freezing or by
	// boiling. bReservoir is how this codebase says bottomless everywhere else --
	// Consume refuses to spend one, and there is a test named for it -- and these
	// were the paths that took ground off a body already agreed to be endless.
	if (!Definition || Volume->bReservoir)
	{
		return false;
	}

	return Super::ConsumeSurfaceArea(Area);
}

bool AARPGFluidPool::AbsorbSurfaceVolume(double InVolume)
{
	if (InVolume <= 0.0 || !Definition || Ring.Num() < 3)
	{
		return false;
	}

	// A POOL BIG ENOUGH TO BE A RESERVOIR TAKES IT AND SHOWS NOTHING, the mirror
	// of refusing to be depleted. A lake gaining a visible ring of shoreline
	// because a floe melted on it is the same wrongness as one shrinking because
	// a fireball hit it.
	if (Volume->bReservoir)
	{
		return true;
	}

	SetRing(ARPGFluidGeometry::GrowToArea(Ring,
		ARPGFluidGeometry::PolygonArea(Ring) + InVolume / FMath::Max(1.f, Definition->Depth)));

	return true;
}

float AARPGFluidPool::GetSurfaceEnergyDensity() const
{
	return Definition ? Definition->EnergyPerArea : 0.f;
}

float AARPGFluidPool::GetSurfaceDensity() const
{
	return Definition ? Definition->Density : 0.f;
}

double AARPGFluidPool::GetMinimumArea() const
{
	return Definition ? Definition->MinimumArea : 0.0;
}

void AARPGFluidPool::RebuildFromRing()
{
	Super::RebuildFromRing();

	if (!Definition)
	{
		return;
	}

	const double Area = GetArea();

	// A RESERVOIR once enough has gathered, rather than a flag on the asset: the
	// same definition describes a splash and a lake, and which one this is falls
	// out of how much is actually here.
	Volume->bReservoir = Area >= Definition->ReservoirArea;

	// Energy scales with area, so a big pool genuinely out-trades a small one
	// when a fireball lands in it rather than every puddle being equally potent.
	Volume->SetEnergy(Area * Definition->EnergyPerArea);

	// Damping in proportion: a fireball hitting a lake should hiss, not explode.
	Volume->Absorption = Volume->bReservoir ? 0.9f : 0.3f;

	// AND IT CARRIES A CHARGE. From the definition rather than left at the
	// volume's default of zero, which is what a puddle silently had: the
	// conduction subsystem filters neighbours on Conductivity > 0, so a chain of
	// real deposited pools dropped out of its own graph entirely. The tests set it
	// by hand, which is exactly why nobody noticed.
	Volume->Conductivity = Definition->Conductivity;
}

// ---------------------------------------------------------------------------
// Solid
// ---------------------------------------------------------------------------

AARPGFluidSolid::AARPGFluidSolid()
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
}

void AARPGFluidSolid::Setup(UARPGSolidDefinition* InDefinition, const TArray<FVector2D>& InRing,
	float InGroundHeight)
{
	Definition = InDefinition;
	GroundHeight = InGroundHeight;

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

float AARPGFluidSolid::GetSurfaceOffset() const
{
	return Definition ? Definition->Thickness : 0.f;
}

float AARPGFluidSolid::GetSurfaceEnergyDensity() const
{
	return Definition ? Definition->EnergyPerArea : 0.f;
}

double AARPGFluidSolid::GetMinimumArea() const
{
	return Definition ? Definition->MinimumArea : 0.0;
}

UMaterialInterface* AARPGFluidSolid::ResolveSurfaceMaterial() const
{
	return Definition ? Definition->SurfaceMaterial.LoadSynchronous() : nullptr;
}

void AARPGFluidSolid::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AARPGFluidSolid, Definition);

	// The RESULT of the buoyancy settle, not its inputs. A client cannot see who
	// is standing on a floe accurately enough to arrive at the same number, and
	// four bytes is cheaper than trying.
	DOREPLIFETIME(AARPGFluidSolid, Draft);

	// The hole matters on the client as much as the outline does: it is a gap you
	// can fall through, so a client that meshed and collided the slab without it
	// would let a player stand on air over the melted-out middle.
	// THE FIELD, which is the slab. Millimetre integers rather than floats for
	// exactly this reason -- see FARPGSolidField -- and even so it is the heaviest
	// thing this system puts on the wire, so a floe replicates at a modest rate
	// and dirty-region updates are the obvious next economy.
	DOREPLIFETIME(AARPGFluidSolid, Field);
}

void AARPGFluidSolid::RebuildFromRing()
{
	// THE TOTALS ARE TRANSIENT, so a client that has just received the field has
	// the cells and none of the sums. Rebuilding is a write-time cost and this is
	// only ever reached on a write -- the per-frame tick reads the cache and never
	// comes through here -- so paying for one sweep is what makes the cache safe
	// to trust everywhere else.
	Field.Refresh();

	if (!Definition || !Field.IsValidField() || Field.IcedCellCount() == 0)
	{
		ARPGFluidGeometry::BuildFieldMesh(Surface, FARPGSolidField(), FVector2D::ZeroVector);
		Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	// FROM THE FIELD, not from an outline. Where the ice is, how thick it is and
	// how high it stands are all one answer now, and the box, the mesh and the
	// collision are three readings of it.
	const FVector2D Centre = Field.IcedCentroid();
	const FVector2D Reach(
		Field.SupportDistance(Centre, FVector2D(1, 0)),
		Field.SupportDistance(Centre, FVector2D(0, 1)));

	SetActorLocation(FVector(Centre.X, Centre.Y, GroundHeight + GetVerticalOffset()));

	Bounds->SetBoxExtent(FVector(
		FMath::Max(1.f, static_cast<float>(Reach.X)),
		FMath::Max(1.f, static_cast<float>(Reach.Y)),
		FMath::Max(1.f, Definition->Thickness * 4.f + 100.f)));

	Volume->SurfaceHeightOffset = Definition->Thickness;

	ARPGFluidGeometry::BuildFieldMesh(Surface, Field, Centre);

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

int32 AARPGFluidSolid::CountOccupants() const
{
	const UWorld* World = GetWorld();
	if (!World || Field.IcedCellCount() == 0)
	{
		return 0;
	}

	const FVector2D Centre = Field.IcedCentroid();
	const FVector2D Extent(
		Field.SupportDistance(Centre, FVector2D(1, 0)),
		Field.SupportDistance(Centre, FVector2D(0, 1)));
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

float AARPGFluidSolid::ComputeTargetDraft() const
{
	if (!Definition || !FloatsOn)
	{
		return 0.f;
	}

	// THE FLUID'S OWN DENSITY, asked rather than assumed. Nothing here names water
	// or ice: a crust on lava and a floe on a pond are the same two numbers
	// compared, and which of them floats is data.
	const float FluidDensity = FloatsOn->GetSurfaceDensity();

	const double Area = Field.IcedArea();
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
	const float AverageThickness = static_cast<float>(Field.IceVolume() / Area);
	const float SlabDraft = AverageThickness * (Definition->Density / FluidDensity);

	const float LoadMass = OccupantCount * Definition->OccupantMass * Definition->LoadResponse;
	const float LoadDraft = LoadMass / (FluidDensity * static_cast<float>(Area));

	// Never further than under. Past this the slab is swamped, and letting it keep
	// sinking would drag whoever is standing on it through the floor.
	return FMath::Min(SlabDraft + LoadDraft, AverageThickness);
}

bool AARPGFluidSolid::HasRoomToward(const FVector2D& Direction) const
{
	if (!FloatsOn || Field.IcedCellCount() == 0)
	{
		return false;
	}

	// Clear water this far past the edge counts as room. Smaller than a floe and
	// larger than one cell of the grid it is measured on.
	static constexpr double Clearance = 50.0;

	const FVector2D Centre = Field.IcedCentroid();
	const double Reach = Field.SupportDistance(Centre, Direction);

	return FloatsOn->IsSurfaceAt(Centre + Direction * (Reach + Clearance));
}

void AARPGFluidSolid::UpdateAnchoring()
{
	bAnchored = false;

	if (!FloatsOn || Field.IcedCellCount() == 0)
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

void AARPGFluidSolid::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Server only. The draft replicates as a result and the outline carries the
	// drift, so a client that simulated its own would be fighting both.
	// IsValid rather than a null check on FloatsOn: an actor destroyed this frame
	// is not garbage collected until later, so the interface still points at it
	// and every query below would run against a dead pool.
	if (!HasAuthority() || !Definition || !IsValid(FloatsOn.GetObject())
		|| Field.IcedCellCount() == 0)
	{
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

	const FVector2D Centre = Field.IcedCentroid();

	// The waterline it should be riding, asked of the body it froze out of -- so a
	// floe on a Water plugin river follows the waves and one on a puddle sits on a
	// flat number, without this knowing which it is on.
	GroundHeight = FloatsOn->GetSurfaceLevelAt(Centre);

	OccupantCount = CountOccupants();

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
		// THE FIELD SLIDES, not its cells. Drifting is one vector add, which is
		// the cheapest operation in the whole system and the reason a floe can be
		// carried every frame rather than four times a second.
		Field.Translate(Step);
		SetActorLocation(GetActorLocation() + FVector(Step.X, Step.Y, 0.f));

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

double AARPGFluidSolid::GetArea() const
{
	// The ice that is actually LEFT, which after a fireball is not the outline it
	// froze with. Energy, buoyancy and retirement all read this.
	return Field.IcedArea();
}

double AARPGFluidSolid::MeltAt(const FVector2D& Where, float Radius, float Depth)
{
	const double Removed = Field.MeltBowl(Where, Radius, Depth);

	if (Removed > 0.0)
	{
		RebuildFromRing();
		UpdateAnchoring();
	}

	return Removed;
}

double AARPGFluidSolid::MeltUniformly(float FromTop, float FromBottom)
{
	const double Removed = Field.MeltUniform(FromTop, FromBottom);

	if (Removed > 0.0)
	{
		RebuildFromRing();
	}

	return Removed;
}

bool AARPGFluidSolid::ConsumeSurfaceArea(double Area)
{
	// AN AREA IS NOT WHAT HAPPENS TO A SLAB. A pool loses ground uniformly because
	// a liquid has no third dimension to lose it in; ice melts WHERE it was hit.
	// The reaction path calls MeltAt with the contact instead, and this remains
	// only for anything that still asks in area -- thinning the whole slab by the
	// depth that much ice would have been.
	const double Plan = Field.IcedArea();
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
	FVector2D MeltedAt = Field.IcedCentroid();

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
	ReturnMeltwater(Melted, MeltedAt);

	return Field.IcedCellCount() == 0 || Field.IcedArea() < GetMinimumArea();
}

void AARPGFluidSolid::ReturnMeltwater(double MeltedVolume, const FVector2D& At)
{
	// Null MeltsInto is correct for obsidian: rock that formed on lava is not
	// frozen lava, and breaking it releases nothing.
	if (MeltedVolume <= 0.0 || !Definition || !Definition->MeltsInto)
	{
		return;
	}

	UARPGFluidDefinition* Fluid = Definition->MeltsInto;

	// MASS IS WHAT IS CONSERVED, not volume. Ice is lighter than the water it came
	// from, so a cubic metre of it does not melt into a cubic metre -- it melts
	// into the volume of water that weighs the same. The same two densities that
	// decide whether the slab floats decide how much water it is worth, which is
	// the point of them being densities rather than a float called Buoyancy.
	const double FluidVolume = MeltedVolume
		* Definition->Density / FMath::Max(KINDA_SMALL_NUMBER, Fluid->Density);

	// BACK INTO WHATEVER IT IS FLOATING ON, first, and the floe never learns which
	// kind of thing that is: a lake takes it and nothing appears, a puddle takes it
	// by growing its outline. Only a slab that has been left on dry land -- its
	// pool evaporated out from under it -- falls through to making a body of its
	// own.
	//
	// IsValid rather than a null check: a pool destroyed this frame has not been
	// garbage collected yet, so the interface still points at it.
	IARPGElementalSurface* Riding = IsValid(FloatsOn.GetObject()) ? FloatsOn.GetInterface() : nullptr;

	if (Riding && Riding->IsSurfaceAt(At) && Riding->AbsorbSurfaceVolume(FluidVolume))
	{
		return;
	}

	UARPGFluidSurfaceSubsystem* Fluids =
		GetWorld() ? GetWorld()->GetSubsystem<UARPGFluidSurfaceSubsystem>() : nullptr;

	if (!Fluids)
	{
		return;
	}

	// THE BED, not the waterline. GroundHeight on a solid tracks the surface it
	// rides, which is where the ice is -- but water lies on the bottom. For a floe
	// melting on a puddle those are centimetres apart and either would do; for one
	// melting on dry land after its pool evaporated, the bed is the only answer
	// that is not in the air.
	const float Bed = Riding ? Riding->GetSurfaceBedAt(At) : GroundHeight;

	Fluids->ReturnFluid(At, Bed, FluidVolume, Fluid);
}

bool AARPGFluidSolid::IsStandableAt(FVector WorldPoint) const
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
	return Field.IsIcedAt(FVector2D(WorldPoint.X, WorldPoint.Y));
}
