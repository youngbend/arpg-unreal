// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGSurfaceBody.h"
#include "SignificanceManager.h"
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

const FName AARPGSurfaceBody::SignificanceTag(TEXT("ARPGSurfaceBody"));

namespace
{
	/**
	 * Distance falloff against the nearest viewpoint, 1 underfoot to 0 at Reach.
	 *
	 * NO VIEWPOINTS MEANS EVERYTHING MATTERS. A dedicated server with no local
	 * viewer, and every automation fixture, would otherwise quietly switch the
	 * whole system off -- the kind of optimisation that shows up only as a test
	 * passing for the wrong reason.
	 */
	float ComputeSurfaceSignificance(USignificanceManager::FManagedObjectInfo* Info,
		const FTransform& Viewpoint)
	{
		const AARPGSurfaceBody* Body = Info ? Cast<AARPGSurfaceBody>(Info->GetObject()) : nullptr;
		if (!IsValid(Body))
		{
			return 0.f;
		}

		// Permanent bodies are authored, not litter: an earth wall someone raised
		// for cover must never be the thing a budget decides to drop.
		if (Body->IsPermanent())
		{
			return TNumericLimits<float>::Max();
		}

		const float Reach = 8000.f;
		const float Distance = FVector::Dist(Body->GetActorLocation(), Viewpoint.GetLocation());
		return FMath::Clamp(1.f - Distance / Reach, 0.f, 1.f);
	}
}

void AARPGSurfaceBody::BeginPlay()
{
	Super::BeginPlay();

	if (USignificanceManager* Significance = USignificanceManager::Get(GetWorld()))
	{
		Significance->RegisterObject(this, SignificanceTag, &ComputeSurfaceSignificance,
			USignificanceManager::EPostSignificanceType::None);
	}
}

void AARPGSurfaceBody::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (USignificanceManager* Significance = USignificanceManager::Get(GetWorld()))
	{
		Significance->UnregisterObject(this);
	}

	Super::EndPlay(EndPlayReason);
}

float AARPGSurfaceBody::GetSignificance() const
{
	const USignificanceManager* Significance = USignificanceManager::Get(GetWorld());
	if (!Significance)
	{
		// No manager -- which is every automation fixture. Everything matters
		// equally, so the budget falls back to area alone.
		return 1.f;
	}

	const USignificanceManager::FManagedObjectInfo* Info = Significance->GetManagedObject(this);
	return Info ? Info->GetSignificance() : 1.f;
}

AARPGSurfaceBody::AARPGSurfaceBody()
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

void AARPGSurfaceBody::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// THE OUTLINE, NOT THE MESH. Everything a client draws and walks on is a pure
	// function of these two, so a floe costs a few dozen FVector2Ds rather than
	// vertex buffers, and server and client geometry are identical by construction
	// instead of by hoping the cook matched.
	DOREPLIFETIME(AARPGSurfaceBody, Ring);
	DOREPLIFETIME(AARPGSurfaceBody, GroundHeight);
}

void AARPGSurfaceBody::OnRep_Body()
{
	RebuildFromRing();
}

const TArray<FVector2D>& AARPGSurfaceBody::GetMeshHole() const
{
	static const TArray<FVector2D> None;
	return None;
}

TArray<FVector2D> AARPGSurfaceBody::GetSurfaceFootprint(const FVector2D& Centre, double Radius) const
{
	return Ring;
}

float AARPGSurfaceBody::GetSurfaceLevelAt(const FVector2D& At) const
{
	return GetSurfaceHeight();
}

bool AARPGSurfaceBody::IsSurfaceAt(const FVector2D& At) const
{
	return ARPGFluidGeometry::PolygonContains(Ring, At);
}

void AARPGSurfaceBody::TranslateRing(const FVector2D& Delta)
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

bool AARPGSurfaceBody::ConsumeSurfaceArea(double Area)
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

void AARPGSurfaceBody::OnElementalReaction_Implementation(float Consumed, float Remaining,
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

int32 AARPGSurfaceBody::GetSurfaceTriangleCount() const
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

UPrimitiveComponent* AARPGSurfaceBody::GetSurfaceComponent() const
{
	return Surface;
}

UMaterialInterface* AARPGSurfaceBody::GetSurfaceMaterial() const
{
	return Surface ? Surface->GetMaterial(0) : nullptr;
}

void AARPGSurfaceBody::SetRing(const TArray<FVector2D>& NewRing)
{
	Ring = NewRing;
	RebuildFromRing();
}

double AARPGSurfaceBody::GetArea() const
{
	return ARPGFluidGeometry::PolygonArea(Ring);
}

bool AARPGSurfaceBody::ContainsPoint(FVector WorldPoint) const
{
	// The POLYGON, not the box. The box is broadphase; this is the truth.
	return ARPGFluidGeometry::PolygonContains(Ring, FVector2D(WorldPoint.X, WorldPoint.Y));
}

void AARPGSurfaceBody::RebuildFromRing()
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
