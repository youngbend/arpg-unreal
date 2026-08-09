// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGFluidBody.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGFluidDefinition.h"
#include "ARPGFluidGeometry.h"
#include "ARPGMagicElement.h"
#include "Components/BoxComponent.h"

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

	if (Definition->bStandable)
	{
		// Blocks rather than overlaps, so a character walks ON it. Note the
		// bounds are a BOX: standing off the polygon but inside the box is
		// possible, which is why anything that cares asks IsStandableAt.
		Bounds->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Bounds->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
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
