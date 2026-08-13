// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGSlabEffects.h"
#include "ARPGDischargeContext.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGFluidGeometry.h"
#include "ARPGFluidSurfaceSubsystem.h"
#include "ARPGMagicElement.h"
#include "ARPGSolidBody.h"
#include "ARPGSolidDefinition.h"
#include "ARPGWorld.h"
#include "Components/DynamicMeshComponent.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"

namespace
{
	/** As high above the caster as the ground probe starts. */
	constexpr float SlabProbeLift = 300.f;

	/** And as far below, before giving up on there being ground at all. */
	constexpr float SlabProbeDrop = 1200.f;
}

// ---------------------------------------------------------------------------
// Raising one
// ---------------------------------------------------------------------------

AARPGRaiseSlabEffect::AARPGRaiseSlabEffect()
{
	// The spell is the eruption, not the rock. It lives its ordinary placeholder
	// life and dies; the slab it left behind is a separate body that outlives it.
}

bool AARPGRaiseSlabEffect::FindGround(const FVector& From, float& OutHeight) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ARPGRaiseSlab), /*bTraceComplex=*/false);
	Params.AddIgnoredActor(this);
	Params.AddIgnoredActor(GetOwner());

	FHitResult Hit;
	const bool bHit = World->LineTraceSingleByChannel(Hit,
		From + FVector(0.f, 0.f, SlabProbeLift),
		From - FVector(0.f, 0.f, SlabProbeDrop),
		ECC_Visibility, Params);

	OutHeight = bHit ? static_cast<float>(Hit.ImpactPoint.Z) : 0.f;
	return bHit;
}

void AARPGRaiseSlabEffect::InitializeFromContext(const FARPGDischargeContext& InContext)
{
	Super::InitializeFromContext(InContext);

	UWorld* World = GetWorld();
	UARPGFluidSurfaceSubsystem* Fluids =
		World ? World->GetSubsystem<UARPGFluidSurfaceSubsystem>() : nullptr;

	if (!Definition || !Fluids)
	{
		if (!Definition)
		{
			UE_LOG(LogARPGWorld, Warning,
				TEXT("'%s' raises a slab but names no solid definition, so it is only a shove."),
				*GetNameSafe(GetClass()));
		}
		return;
	}

	// SERVER ONLY, like every other body. The slab replicates down as an outline
	// and a client raising its own would leave two walls where the player made
	// one -- the same rule the deposit path already lives by.
	if (World->GetNetMode() == NM_Client)
	{
		return;
	}

	const FVector Facing = InContext.Direction.GetSafeNormal2D();
	const FVector Centre = InContext.Origin + Facing * Standoff;

	float GroundHeight = 0.f;
	if (!FindGround(Centre, GroundHeight))
	{
		// Cast over a ledge or out over water. There is no ground to raise, which
		// is a real outcome rather than a failure -- the same answer a spell gets
		// when it tries to wet a floor that is not there.
		UE_LOG(LogARPGWorld, Verbose, TEXT("Nothing under the cast to raise a slab from."));
		return;
	}

	// The charge buys SIZE, because the slab is the payload. A charge that only
	// brightened the flash would be a charge that did nothing.
	const float Scale = FMath::Lerp(1.f, ChargeScale, InContext.GetNormalisedPower());
	const float Reach = Extent * Scale;

	const FVector2D At(Centre.X, Centre.Y);
	const FVector2D Across(-Facing.Y, Facing.X);

	TArray<FVector2D> Ring;

	switch (Shape)
	{
	case EARPGSlabShape::Wall:
		// A BAR ACROSS THE FACING, which is a stadium swept sideways rather than a
		// long thin box -- the same helper a jet's footprint uses, so the ends are
		// rounded and the offsetter never sees a sharp corner to round badly.
		Ring = ARPGFluidGeometry::MakeStadium(At - Across * Reach, At + Across * Reach,
			Depth * 0.5);
		break;

	case EARPGSlabShape::Ring:
		// AROUND the caster, which is what an emanation emanates. A disc, because
		// a genuine annulus is a polygon with a hole and a body here is one outer
		// ring -- and a filled disc of rock you stand on top of is the better
		// answer anyway.
		Ring = ARPGFluidGeometry::MakeCircle(FVector2D(InContext.Origin.X, InContext.Origin.Y),
			Reach, 24);
		break;

	case EARPGSlabShape::Pillar:
	default:
		Ring = ARPGFluidGeometry::MakeCircle(At, Reach);
		break;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	Raised = World->SpawnActor<AARPGSolidBody>(AARPGSolidBody::StaticClass(),
		FTransform(FVector(At.X, At.Y, GroundHeight)), Params);

	if (!Raised)
	{
		return;
	}

	Raised->Setup(Definition, Ring, GroundHeight);

	// FloatsOn is left NULL, which is the whole of "rooted". The buoyancy tick
	// reads it and finds nothing, so the slab never settles, never drifts and
	// never asks the ground for a waterline it does not have.

	// Buried to its own full thickness, so it climbs out of the ground rather
	// than appearing. Draft already means "how far under its resting height this
	// is sitting", so this IS the animation.
	Raised->BeginBuried(Definition->Thickness);

	// AND ON THE REGISTER, because freezing used to be the only way to make one.
	// Unregistered it would still draw, collide and react -- but a bolt striking
	// the water it stands in would conduct as though the wall were not there.
	Fluids->RegisterSolid(Raised);

	UE_LOG(LogARPGWorld, Verbose, TEXT("Raised a slab of '%s' at %s."),
		*Definition->GetElementTag().ToString(), *Centre.ToCompactString());
}

// ---------------------------------------------------------------------------
// Throwing one
// ---------------------------------------------------------------------------

AARPGLaunchSlabProjectile::AARPGLaunchSlabProjectile()
{
	// EMPTY UNTIL SOMETHING IS THROWN. Most casts of this spell are a plain
	// conjured boulder and never touch it, so it costs a component and no
	// geometry -- the mesh is built exactly once, at the moment a slab is picked
	// up, and never edited afterwards.
	Carriage = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("Carriage"));
	Carriage->SetupAttachment(RootComponent);
	Carriage->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Carriage->SetCastShadow(true);

	// Tumbling needs a tick; a conjured boulder does not, and finds out on the
	// first frame that it has nothing to turn.
	PrimaryActorTick.bCanEverTick = true;
}

void AARPGLaunchSlabProjectile::InitializeFromContext(const FARPGDischargeContext& InContext)
{
	Super::InitializeFromContext(InContext);

	UWorld* World = GetWorld();
	UARPGFluidSurfaceSubsystem* Fluids =
		World ? World->GetSubsystem<UARPGFluidSurfaceSubsystem>() : nullptr;

	if (!Fluids || !InContext.PrimaryElement || World->GetNetMode() == NM_Client)
	{
		return;
	}

	const FVector Ahead =
		InContext.Origin + InContext.Direction.GetSafeNormal2D() * Reach;

	// OF MY OWN ELEMENT. An earth spell throws earth; a wall of ice in front of
	// the caster is somebody else's cover and not this spell's ammunition.
	AARPGSolidBody* Slab =
		Fluids->FindSolidNear(Ahead, Reach, InContext.PrimaryElement->ElementTag);

	if (!Slab)
	{
		// The ordinary case, and not a failure: with nothing to throw this is a
		// plain conjured boulder, exactly as its parent class made it.
		return;
	}

	// SIZED FROM THE ROCK THAT WENT. A thrown wall is a bigger boulder, not a
	// different spell -- so the volume decides the radius and the cap decides how
	// much bigger a spell is allowed to get for free.
	const double Volume = Slab->Field.SolidVolume();
	const double AsSphere = FMath::Pow(FMath::Max(1.0, Volume) * 3.0 / (4.0 * PI), 1.0 / 3.0);

	const float Conjured = FMath::Max(1.f, Radius);
	const float Grown = FMath::Clamp(static_cast<float>(AsSphere),
		Conjured, Conjured * MaxLaunchScale);

	// Three things off one number, the rule this whole family lives by: what it
	// hits, what it leaves, and what it reacts with.
	Radius = Grown;
	Hitbox->TraceRadius = Grown;
	DepositRadius = Grown * DepositSpread;
	ReactionRadius = Grown;

	// Damage in proportion, so throwing a wall is worth having raised one. Read
	// off the ratio taken BEFORE Radius was overwritten, and scaled by AREA
	// rather than volume: the cube would make a big slab absurd, and what a rock
	// does on impact is nearer how much of it hits you than how much there is.
	//
	// Straight onto the hitbox, because the context is what the base class
	// already stamped into BaseDamage -- rewriting the context now would be
	// changing a number nothing reads again.
	const float Bigger = FMath::Square(Grown / Conjured);
	Hitbox->BaseDamage *= Bigger;

	// And the volume with it, since energy and damage are deliberately the same
	// number -- a bigger rock has more of itself to spend on a reaction too.
	if (Volume)
	{
		Volume->SetEnergy(Volume->GetEnergy() * Bigger);
	}

	// TAKE THE ROCK WITH IT, rather than a note of how big it was. The field is
	// snapshotted whole, so what lands is what was thrown -- melt scars and all --
	// and the mesh is baked ONCE here and never touched again. Everything that
	// makes a mesh the wrong representation for a slab standing in the world is an
	// argument about repeated boolean editing; a mesh built once and discarded has
	// none of it, and it is the only thing that can tumble.
	Carried = Slab->Field;
	CarriedDefinition = Slab->Definition;

	ARPGFluidGeometry::BuildFieldMesh(Carriage, Carried, Carried.SolidCentroid());

	if (UMaterialInterface* Rock = Slab->GetSurfaceMaterial())
	{
		Carriage->SetMaterial(0, Rock);
	}

	// AND THE WALL IS GONE, which is the cost of the spell: cover you throw is
	// cover you no longer have. Unregistered explicitly rather than left to the
	// next sweep, because the register is walked by conduction the same frame.
	Fluids->UnregisterSolid(Slab);
	Slab->Destroy();

	bLaunchedFromSlab = true;

	UE_LOG(LogARPGWorld, Verbose,
		TEXT("Threw a slab of '%s' (%.0f cubic cm) instead of conjuring a boulder."),
		*InContext.PrimaryElement->ElementTag.ToString(), Volume);
}

void AARPGLaunchSlabProjectile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// THE WHOLE REASON THE MESH IS HERE. A heightfield's columns run along world
	// Z, so a slab can yaw and keep every one of them vertical -- but a thrown
	// rock pitches and rolls, and that is the rotation z = f(x,y) has no way to
	// hold. In the air it is a mesh, and a mesh turns any way it likes.
	if (bLaunchedFromSlab && Carriage)
	{
		Carriage->AddLocalRotation(FRotator(
			TumbleRate.Y * DeltaTime, TumbleRate.Z * DeltaTime, TumbleRate.X * DeltaTime));
	}
}

AARPGSolidBody* AARPGLaunchSlabProjectile::PutDown(const FVector& Where)
{
	UWorld* World = GetWorld();

	if (bPutDown || !bLaunchedFromSlab || !bLandsAsSlab || !CarriedDefinition
		|| !World || World->GetNetMode() == NM_Client)
	{
		return nullptr;
	}

	bPutDown = true;

	UARPGFluidSurfaceSubsystem* Fluids = World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	if (!Fluids || Carried.SolidCellCount() == 0)
	{
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AARPGSolidBody* Landed = World->SpawnActor<AARPGSolidBody>(
		AARPGSolidBody::StaticClass(), FTransform(Where), Params);

	if (!Landed)
	{
		return nullptr;
	}

	// THE FIELD ITSELF, not a ring it was built from. Setup seeds from an outline
	// and would hand back a fresh unmarked slab; adopting the snapshot is what
	// makes the pillar that lands the pillar that was thrown.
	Landed->AdoptField(CarriedDefinition, Carried, Where,
		static_cast<float>(GetActorRotation().Yaw));

	Fluids->RegisterSolid(Landed);

	UE_LOG(LogARPGWorld, Verbose, TEXT("A thrown slab of '%s' came to rest at %s."),
		*CarriedDefinition->GetElementTag().ToString(), *Where.ToCompactString());

	return Landed;
}

void AARPGLaunchSlabProjectile::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// WHERE IT STOPPED IS WHERE IT LANDS. A projectile ends by hitting something
	// or by expiring, and both are the rock coming to rest -- there is no third
	// outcome worth a branch. Only on Destroyed, so tearing down a level does not
	// scatter walls across it on the way out.
	if (EndPlayReason == EEndPlayReason::Destroyed)
	{
		FVector Ground = GetActorLocation();

		if (UARPGFluidSurfaceSubsystem* Fluids =
				GetWorld() ? GetWorld()->GetSubsystem<UARPGFluidSurfaceSubsystem>() : nullptr)
		{
			// Reuse the deposit probe, which already knows to trace past every body
			// this subsystem owns -- including, importantly, the one this rock is
			// about to become.
			Ground.Z = Fluids->GroundUnder(Ground, this);
		}

		PutDown(Ground);
	}

	Super::EndPlay(EndPlayReason);
}
