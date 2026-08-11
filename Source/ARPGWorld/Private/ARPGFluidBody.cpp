// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGFluidBody.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGFluidDefinition.h"
#include "ARPGFluidGeometry.h"
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

	SetActorLocation(FVector(Centre.X, Centre.Y, GroundHeight));

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

TArray<FVector2D> AARPGFluidPool::GetFreezableFootprint(const FVector2D& Centre, double Radius) const
{
	return Ring;
}

float AARPGFluidPool::GetFreezableSurfaceHeight(const FVector2D& At) const
{
	return GetSurfaceHeight();
}

bool AARPGFluidPool::ConsumeFreezableArea(double Area)
{
	if (!Definition)
	{
		return false;
	}

	// A POOL BIG ENOUGH TO BE A RESERVOIR IS NOT DEPLETED EITHER. bReservoir is
	// how this codebase says bottomless everywhere else -- Consume refuses to
	// spend one, and there is a test named for it -- and freezing was the single
	// path that took area off a body it had already agreed could not run out.
	// Freeze a lake enough times and it used to vanish.
	if (Volume->bReservoir)
	{
		return false;
	}

	// The fluid is genuinely USED UP. Subtracting the frozen region would leave a
	// hole, and a liquid flows back over a hole -- so the pool keeps its shape and
	// loses the area instead, which is what ShrinkToArea is for.
	const double Remaining = FMath::Max(0.0, GetArea() - Area);

	if (Remaining < Definition->MinimumArea)
	{
		return true;
	}

	SetRing(ARPGFluidGeometry::ShrinkToArea(Ring, Remaining));
	return false;
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
}

// ---------------------------------------------------------------------------
// Solid
// ---------------------------------------------------------------------------

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

UMaterialInterface* AARPGFluidSolid::ResolveSurfaceMaterial() const
{
	return Definition ? Definition->SurfaceMaterial.LoadSynchronous() : nullptr;
}

void AARPGFluidSolid::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AARPGFluidSolid, Definition);

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

	// A slab is a thing you stand on, not a body you react with, so its volume
	// carries no energy -- it is there to be conducted through and to be seen,
	// not to trade in a collision.
	Volume->SetEnergy(0.f);
	Volume->bReservoir = false;
	Volume->bAmbientSource = false;

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
