// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGFluidRegion.h"

#include "ARPGElementalVolumeComponent.h"
#include "ARPGFluidDefinition.h"
#include "ARPGFluidGeometry.h"
#include "ARPGFluidPresentationSubsystem.h"
#include "ARPGFluidSurfaceSubsystem.h"
#include "ARPGMagicElement.h"
#include "Components/BoxComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"

AARPGFluidRegion::AARPGFluidRegion()
{
	PrimaryActorTick.bCanEverTick = false;

	// NOT REPLICATED -- see the header. The field is not replicated, so there is
	// nothing here a client could not derive, and sending a proxy would be sending
	// a consequence of the simulation rather than the simulation.
	bReplicates = false;

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

	// Water is not a thing you stand on.
	Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// A PUDDLE IS NOT A LANDMARK -- the same cull the pool carried, for the same
	// reason: at range a body of water is a few pixels of tinted ground.
	Surface->SetCullDistance(15000.f);
}

void AARPGFluidRegion::UpdateFrom(FARPGFluidField* InField,
	const FARPGFluidField::FRegion& Region, UARPGFluidDefinition* InDefinition)
{
	Field = InField;
	Definition = InDefinition;

	Layer = Region.Layer;
	Area = Region.Area;
	WaterVolume = Region.Volume;
	MaxLevel = Region.MaxLevel;
	MinBed = Region.MinBed;
	Centroid = Region.Centroid;

	Cells = Region.Lookup;

	if (Field)
	{
		Field->BuildRegionOutline(Region, Ring);
	}

	const FBox2D Box = Region.Bounds;
	const FVector2D Extent = Box.GetExtent();

	SetActorLocation(FVector(Centroid.X, Centroid.Y, MinBed));

	// Generous headroom above the surface, so a spell arriving from above enters
	// the broadphase well before it reaches the waterline -- which is what lets
	// the reaction subsystem wait until it has actually arrived. And down to the
	// deepest bed under the body, because a puddle running down a ramp reaches
	// metres below where its middle sits.
	const float Depth = FMath::Max(1.f, MaxLevel - MinBed);

	Bounds->SetBoxExtent(FVector(
		FMath::Max(1.f, Extent.X),
		FMath::Max(1.f, Extent.Y),
		FMath::Max(1.f, Depth * 2.f + 100.f)));

	Volume->SurfaceHeightOffset = MaxLevel - MinBed;

	if (Definition)
	{
		Volume->Element = Definition->Element;

		// A THRESHOLD, NOT A FLAG. The same asset describes a splash and a lake,
		// and which one this is falls out of how much has gathered -- measured on
		// the CELLS, because a body flowing round a pillar must not be credited
		// with the pillar.
		Volume->bReservoir = Area >= Definition->ReservoirArea;
		Volume->SetEnergy(Area * Definition->EnergyPerArea);
		Volume->Absorption = Volume->bReservoir ? 0.9f : 0.3f;

		// Set from the definition rather than left at the component's default of
		// zero, which is what once made real deposited puddles silently refuse to
		// conduct while the conduction tests, which set it by hand, passed.
		Volume->Conductivity = Definition->Conductivity;
	}

	// --- The stopgap surface ------------------------------------------------
	//
	// FLAT, AND BLOCKY, AND TEMPORARY. Drawn from the traced ring at the body's
	// own level so there is something to look at when the Niagara sheet is not
	// running. No bed sampler: a single flat cap over a body that follows a ramp
	// is wrong, and it is wrong in a way nobody should be tempted to fix here
	// rather than by finishing the sheet.
	//
	// AND IT HIDES WHEN THE SHEET IS ON, rather than being deleted with it. The
	// two would otherwise both be drawn, z-fighting a couple of centimetres apart;
	// and deleting it would mean that switching the sheet OFF -- which is how you
	// prove gameplay does not depend on it -- left the water invisible, so nobody
	// would ever switch it off.
	if (Surface)
	{
		Surface->SetVisibility(!bDrawnBySheet);

		ARPGFluidGeometry::BuildSlabMesh(Surface, Ring, TArray<FVector2D>(), Centroid,
			/*BottomZ=*/0.f, /*TopZ=*/FMath::Max(0.f, MaxLevel - MinBed));

		if (Definition)
		{
			if (UMaterialInterface* Material = Definition->SurfaceMaterial.LoadSynchronous())
			{
				Surface->SetMaterial(0, Material);
			}
		}
	}
}

void AARPGFluidRegion::SetDrawnBySheet(bool bDrawn)
{
	if (bDrawnBySheet == bDrawn)
	{
		return;
	}

	bDrawnBySheet = bDrawn;

	if (Surface)
	{
		Surface->SetVisibility(!bDrawnBySheet);
	}
}

void AARPGFluidRegion::RefreshMetrics()
{
	if (!Field)
	{
		return;
	}

	const float Reach = GetReach();

	Area = Field->GetAreaOfCells(Cells);
	WaterVolume = Field->GetVolumeOfCells(Cells);

	if (Volume && Definition)
	{
		Volume->bReservoir = Area >= Definition->ReservoirArea;
		Volume->SetEnergy(Area * Definition->EnergyPerArea);
	}
}

float AARPGFluidRegion::GetReach() const
{
	// Far enough to cover the body, so a consume aimed at the middle can find
	// water anywhere in it, and no further -- taking water out of the puddle next
	// door would be a reaction reaching across dry ground.
	if (Ring.Num() >= 3)
	{
		return static_cast<float>(ARPGFluidGeometry::PolygonBounds(Ring).GetExtent().Size()) + 1.f;
	}

	return Field ? Field->GetCellSize() * 2.f : 100.f;
}

TArray<FVector2D> AARPGFluidRegion::GetSurfaceFootprint(const FVector2D& Centre,
	double Radius) const
{
	// HANDED BACK WHOLE. A body is small enough to, and the caller clips against
	// its own agent anyway -- exactly as AARPGSurfaceBody did. A reservoir is the
	// case that cannot, and a reservoir is still a water body component.
	return Ring;
}

float AARPGFluidRegion::GetSurfaceLevelAt(const FVector2D& At) const
{
	return Field ? Field->SampleLevel(At, Layer) : MaxLevel;
}

float AARPGFluidRegion::GetSurfaceBedAt(const FVector2D& At) const
{
	return Field ? Field->SampleBed(At, Layer) : MinBed;
}

bool AARPGFluidRegion::IsSurfaceAt(const FVector2D& At) const
{
	// THE CELLS, NOT THE RING. Water round a pillar has an outline that encloses
	// the pillar, and the pillar is not somewhere you are in the water.
	//
	// AND THIS BODY'S CELLS, not the field's. Every body of one element shares one
	// field, so asking the field alone answers "is there water anywhere here",
	// which every puddle in the world would say yes to about every other one.
	return Field && Field->IsWetAt(At, Layer) && OwnsCellAt(At);
}

bool AARPGFluidRegion::OwnsCellAt(const FVector2D& At) const
{
	if (!Field)
	{
		return false;
	}

	FIntPoint Coord;
	int32 Index = 0;
	Field->ResolveCellPublic(At, Coord, Index);

	// EVERY FLOOR AT THIS CELL, and whichever one is ours is the answer. A slot
	// carries the floor as well as the square of ground, so the balcony and the
	// room under it never answer for each other -- and looking through all of them
	// rather than trusting this body's own layer number means it still works where
	// this floor happened to be the second one allocated at that cell.
	for (int32 Which = 0; Which < Field->GetLayers(); ++Which)
	{
		if (Cells.Contains(TPair<FIntPoint, int32>(Coord, Field->SlotOf(Which, Index))))
		{
			return true;
		}
	}

	return false;
}

FVector2D AARPGFluidRegion::GetSurfaceFlowAt(const FVector2D& At) const
{
	// THE FIRST REAL ANSWER THIS HAS EVER GIVEN. AARPGSurfaceBody returned zero
	// unconditionally -- a puddle genuinely had no current, because nothing in the
	// polygon model could produce one. A floe on a field drifts because the water
	// under it is actually moving.
	return Field ? Field->SampleVelocity(At, Layer) : FVector2D::ZeroVector;
}

float AARPGFluidRegion::GetSurfaceDensity() const
{
	return Definition ? Definition->Density : 0.f;
}

float AARPGFluidRegion::GetSurfaceEnergyDensity() const
{
	return Definition ? Definition->EnergyPerArea : 0.f;
}

bool AARPGFluidRegion::ConsumeSurfaceArea(double InArea)
{
	if (!Field || !Definition || InArea <= 0.0)
	{
		return false;
	}

	// A LAKE IS BOTTOMLESS, so freezing and boiling both take nothing from it --
	// the same answer AARPGFluidPool gave, and the same one Consume gives a
	// reservoir volume everywhere else in the codebase.
	if (Volume && Volume->bReservoir)
	{
		return false;
	}

	// GROUND, NOT VOLUME. A reaction's spend converts into AREA -- that is what
	// GetSurfaceEnergyDensity means -- and turning it back into a volume to remove
	// requires guessing how deep the water is. The nominal depth is only ever
	// approximately right, and the error is one-signed: a fireball worth exactly
	// one puddle removed 98% of one and left a rim that no amount of further fire
	// would ever quite finish.
	Field->RemoveAreaFromCells(Cells, Centroid, InArea);
	RefreshMetrics();

	// MEASURED AFTERWARDS RATHER THAN PREDICTED. The caller is asking whether this
	// is still worth being a body, and the honest answer is however much is
	// actually left -- not a subtraction from a total that was last refreshed a
	// quarter of a second ago and has had water flow into it since.
	//
	// AND MEASURED IN GROUND, which is the same test ReconcileRegions applies when
	// it decides what bodies exist at all. Answering this one in VOLUME and that
	// one in AREA meant the two could disagree: a body could report itself spent
	// to the reaction that emptied it and still be found next pass, or the other
	// way about, depending on nothing more than how deep the water happened to
	// have settled.
	return IsSpent();
}

bool AARPGFluidRegion::IsSpent() const
{
	return !Definition || Area < Definition->MinimumArea;
}

bool AARPGFluidRegion::ConsumeSurfaceRegion(const TArray<FVector2D>& InRegion)
{
	if (!Field || !Definition || InRegion.Num() < 3)
	{
		return false;
	}

	if (Volume && Volume->bReservoir)
	{
		return false;
	}

	// EXACTLY THE CELLS IT COVERS, and this is where "no holes" finally lands.
	// AARPGSurfaceBody had to choose between keeping a hole it could not draw and
	// shrinking the pool somewhere it had not been touched; there is no choice to
	// make here, because a gap is a cell with a flag on it.
	Field->ConsumeCellsIn(Cells, InRegion);
	RefreshMetrics();

	return IsSpent();
}

bool AARPGFluidRegion::AbsorbSurfaceVolume(double InVolume)
{
	if (!Field || !Definition || InVolume <= 0.0)
	{
		return false;
	}

	Field->PourVolume(Centroid, MaxLevel, InVolume, GetReach());
	RefreshMetrics();

	return true;
}

void AARPGFluidRegion::OnElementalReaction_Implementation(float Consumed, float Remaining,
	UARPGMagicElement* Product)
{
	const float Density = GetSurfaceEnergyDensity();

	// Amplified rather than spent, or a body nothing can eat. Either way there is
	// no ground to take, and saying so here is what keeps the default projectile
	// reaction -- scale the actor, destroy it at zero -- away from a body of water.
	if (Consumed <= 0.f || Density <= 0.f)
	{
		return;
	}

	// THE SPEND, CONVERTED BACK INTO GROUND. Energy is area times density, so the
	// combination table's consumption rates already decide how fast a fireball
	// eats a puddle, with no second set of numbers to keep in step.
	//
	// AND IF THAT WAS THE LAST OF IT, THE BODY GOES NOW rather than at the next
	// reconcile. Ordinarily the reconcile is the only thing that decides what
	// bodies there are, and it still is -- this is the same decision made a
	// quarter of a second early, because everything that happens after a fireball
	// boils the last of a puddle happens in this frame, including whoever asks how
	// many bodies of water are left.
	if (ConsumeSurfaceArea(Consumed / Density))
	{
		if (UARPGFluidSurfaceSubsystem* Fluids =
				GetWorld() ? GetWorld()->GetSubsystem<UARPGFluidSurfaceSubsystem>() : nullptr)
		{
			Fluids->RetireRegion(this);
		}
	}
}

void AARPGFluidRegion::Drain()
{
	if (!Field || Ring.Num() < 3)
	{
		return;
	}

	Field->ConsumeCellsIn(Cells, Ring);
	RefreshMetrics();
}

bool AARPGFluidRegion::ContainsPoint(FVector WorldPoint) const
{
	if (!Field)
	{
		return false;
	}

	const FVector2D At(WorldPoint.X, WorldPoint.Y);

	if (!Field->IsWetAt(At, Layer) || !OwnsCellAt(At))
	{
		return false;
	}

	// AND AT THE RIGHT HEIGHT. The field is one depth per XY, so a puddle on a
	// balcony and the floor beneath it are the same cell -- which is the one thing
	// a heightfield cannot say and the polygon model could. Until layers land, the
	// bed comparison is what keeps a spell landing downstairs out of the puddle
	// upstairs.
	const float Bed = Field->SampleBed(At, Layer);
	const float Level = Bed + Field->SampleDepth(At, Layer);

	return WorldPoint.Z >= Bed - 100.0 && WorldPoint.Z <= Level + 100.0;
}

int32 AARPGFluidRegion::GetSurfaceTriangleCount() const
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

UPrimitiveComponent* AARPGFluidRegion::GetSurfaceComponent() const
{
	return Surface;
}

UMaterialInterface* AARPGFluidRegion::GetSurfaceMaterial() const
{
	return Surface ? Surface->GetMaterial(0) : nullptr;
}
