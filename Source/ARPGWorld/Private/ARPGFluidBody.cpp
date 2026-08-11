// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGFluidBody.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGFluidDefinition.h"
#include "ARPGFluidGeometry.h"
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

float AARPGFluidPool::GetSurfaceEnergyDensity() const
{
	return Definition ? Definition->EnergyPerArea : 0.f;
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

	SetRing(InRing);
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
	DOREPLIFETIME(AARPGFluidSolid, HoleRing);
}

void AARPGFluidSolid::RebuildFromRing()
{
	Super::RebuildFromRing();

	if (!Definition)
	{
		return;
	}

	// A SLAB CAN BE MELTED, so it carries energy in proportion to its area exactly
	// as a pool does. This used to be flatly zero -- "a thing you stand on, not a
	// body you react with" -- and the consequence was that Resolve bailed at its
	// own guard against zero-energy volumes, so a fireball thrown at an ice floe
	// did nothing at all. Melting was purely a matter of waiting.
	//
	// Still not an ambient source: standing on ice does not wet you.
	Volume->SetEnergy(GetArea() * GetSurfaceEnergyDensity());
	Volume->bReservoir = false;
	Volume->bAmbientSource = false;

	// AND IT DOES NOT CARRY A CHARGE, stated rather than left to the volume's
	// default, because the value matters and the reason is not obvious. A floe is
	// how you cross an electrified river safely: NOT THROUGH THE ICE is a rule
	// about the slab ROOFING the water, and a conductive slab would carry the bolt
	// into the pool it is floating on and defeat its own point.
	Volume->Conductivity = 0.f;

	if (!Definition->bStandable)
	{
		Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	// THE DRAWN SURFACE IS THE WALKABLE ONE. This used to block pawns with the
	// BOUNDS BOX, which is the polygon's rectangle -- so a player could stand off
	// the floe and inside the box, in mid-air over open water. That was tolerable
	// only while nothing was drawn; the moment the slab is visible, the gap
	// between what you see and what holds you up is the bug you notice first.
	//
	// Complex-as-simple, because a slab with an eroded hole through it is not
	// convex and there is nothing to approximate it with that keeps the hole.
	Surface->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Surface->SetCollisionObjectType(ECC_WorldDynamic);
	Surface->SetCollisionResponseToAllChannels(ECR_Block);
	Surface->EnableComplexAsSimpleCollision();

	// Recooked on every melt tick, which is 4Hz per floe by default. Cheap for a
	// slab of a few hundred triangles, and the alternative -- collision lagging
	// the visible edge as it erodes -- is a player standing on water.
	Surface->UpdateCollision(/*bOnlyIfPending=*/false);
}

// ---------------------------------------------------------------------------
// Floating
// ---------------------------------------------------------------------------

int32 AARPGFluidSolid::CountOccupants() const
{
	const UWorld* World = GetWorld();
	if (!World || Ring.Num() < 3)
	{
		return 0;
	}

	const FBox2D Box = ARPGFluidGeometry::PolygonBounds(Ring);
	const FVector2D Extent = Box.GetExtent();
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
		FVector(Box.GetCenter().X, Box.GetCenter().Y, Top + StandingSlice * 0.5f),
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
	if (!Definition)
	{
		return 0.f;
	}

	// Water, in kg per cubic centimetre. Not a knob: it is the thing every other
	// density in the system is relative to.
	static constexpr float WaterDensity = 0.001f;

	const double Area = GetArea();
	if (Area <= 0.0)
	{
		return 0.f;
	}

	// ARCHIMEDES. A floating body displaces its own weight, so the depth it rides
	// at is mass over the water it has to push aside:
	//
	//     Draft = (SlabMass + LoadMass) / (WaterDensity * Area)
	//
	// The slab's own term reduces to Thickness * Density / WaterDensity, with the
	// area cancelling -- which is why a big floe and a small one of the same
	// thickness ride at the same depth, and correctly so. The load's term does NOT
	// cancel, so a wider floe takes a person's weight better. Both fall out of the
	// equation rather than being arranged.
	const float SlabDraft = Definition->Thickness * (Definition->Density / WaterDensity);

	const float LoadMass = OccupantCount * Definition->OccupantMass * Definition->LoadResponse;
	const float LoadDraft = LoadMass / (WaterDensity * static_cast<float>(Area));

	// Never further than under. Past this the slab is swamped, and letting it
	// keep sinking would drag whoever is standing on it through the floor.
	return FMath::Min(SlabDraft + LoadDraft, Definition->Thickness);
}

bool AARPGFluidSolid::HasRoomToward(const FVector2D& Direction) const
{
	if (!FloatsOn || Ring.Num() < 3)
	{
		return false;
	}

	// Clear water this far past the edge counts as room. Smaller than a floe and
	// larger than the wobble in a polygon's outline.
	static constexpr double Clearance = 50.0;

	const FVector2D Centre = ARPGFluidGeometry::PolygonCentroid(Ring);

	// The support function: how far the outline reaches along this direction.
	double Reach = 0.0;
	for (const FVector2D& Point : Ring)
	{
		Reach = FMath::Max(Reach, FVector2D::DotProduct(Point - Centre, Direction));
	}

	return FloatsOn->IsSurfaceAt(Centre + Direction * (Reach + Clearance));
}

void AARPGFluidSolid::UpdateAnchoring()
{
	bAnchored = false;

	if (!FloatsOn || Ring.Num() < 3)
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
	if (!HasAuthority() || !Definition || !FloatsOn || Ring.Num() < 3)
	{
		return;
	}

	const FVector2D Centre = ARPGFluidGeometry::PolygonCentroid(Ring);

	// The waterline it should be riding, asked of the body it froze out of -- so a
	// floe on a Water plugin river follows the waves and one on a puddle sits on a
	// flat number, without this knowing which it is on.
	GroundHeight = FloatsOn->GetSurfaceLevelAt(Centre);

	OccupantCount = CountOccupants();

	// SETTLE toward it rather than snapping. A step change would teleport anyone
	// standing on the slab, and the lag is most of what makes the ice feel like it
	// gives under a footfall rather than being a lift.
	Draft = FMath::FInterpTo(Draft, ComputeTargetDraft(), DeltaTime, Definition->SettleSpeed);

	SetActorLocation(FVector(Centre.X, Centre.Y, GroundHeight - Draft));

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
		TranslateRing(Step);

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

bool AARPGFluidSolid::IsStandableAt(FVector WorldPoint) const
{
	if (!Definition || !Definition->bStandable || !ContainsPoint(WorldPoint))
	{
		return false;
	}

	// Inside the outline AND not down a hole. A floe with a melted-through gap
	// is a floe with a gap you can fall into -- which is the whole reason a
	// solid keeps its holes where a pool discards them.
	return !ARPGFluidGeometry::PolygonContains(HoleRing, FVector2D(WorldPoint.X, WorldPoint.Y));
}
