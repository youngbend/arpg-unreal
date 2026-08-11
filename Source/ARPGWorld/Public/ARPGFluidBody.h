// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ARPGElementalReactive.h"
#include "ARPGElementalSurface.h"
#include "ARPGFluidBody.generated.h"

class UARPGElementalVolumeComponent;
class UARPGFluidDefinition;
class UARPGSolidDefinition;
class UBoxComponent;
class UDynamicMeshComponent;
class UMaterialInterface;
class UPrimitiveComponent;

/**
 * Shared behaviour of anything that IS an outline lying on the ground.
 *
 * A BODY IS ITS POLYGON. The ring (world XY, one simple outer ring) is the
 * single source of truth, and everything else is rebuilt from it whenever it
 * changes: the trigger bounds, the elemental volume's energy, whether it counts
 * as a reservoir, and the mesh you can see. Storing anything derived alongside it
 * is how the two get out of step.
 *
 * THE RING IS ALSO THE ONLY THING THAT REPLICATES. Nothing sends mesh data: a
 * client receives the outline and rebuilds the identical surface and collision
 * from it, because both are pure functions of the polygon. That is what makes a
 * floe two players can stand on cost a few dozen FVector2Ds on the wire.
 */
UCLASS(Abstract)
class ARPGWORLD_API AARPGFluidBody : public AActor, public IARPGElementalSurface,
	public IARPGElementalReactive
{
	GENERATED_BODY()

public:
	AARPGFluidBody();

	//~ IARPGElementalSurface. A body is small enough to hand back whole, so the
	// contact bound is ignored -- the caller clips against the agent anyway.
	virtual TArray<FVector2D> GetSurfaceFootprint(const FVector2D& Centre, double Radius) const override;
	virtual float GetSurfaceLevelAt(const FVector2D& At) const override;
	virtual bool ConsumeSurfaceArea(double Area) override;
	virtual float GetSurfaceEnergyDensity() const override { return 0.f; }
	//~ End IARPGElementalSurface

	/**
	 * A REACTION TAKES GROUND, NOT SCALE.
	 *
	 * Without this hook a body gets UARPGElementalVolumeComponent's default
	 * reaction, which is written for a projectile: it scales the actor down by the
	 * cube root of what is left, and destroys it outright once spent. Both are
	 * wrong here and the second is worse than wrong -- a scaled pool has its mesh,
	 * its trigger box and its outline disagreeing within a frame, and a pool
	 * destroyed from inside Consume leaves the subsystem still holding it.
	 *
	 * What a fireball does to a puddle is boil some of it away, so that is what
	 * this does: convert the energy spent into area at the body's own density and
	 * take it off.
	 */
	virtual void OnElementalReaction_Implementation(float Consumed, float Remaining,
		UARPGMagicElement* Product) override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** The outline, in world XY. Setting it rebuilds everything derived. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	void SetRing(const TArray<FVector2D>& NewRing);

	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	const TArray<FVector2D>& GetRing() const { return Ring; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	double GetArea() const;

	/**
	 * Is this world point genuinely inside?
	 *
	 * THE COLLIDER IS NOT THE BODY. The trigger box is a rectangle while the
	 * body is an arbitrary polygon -- deliberately, because the box only decides
	 * WHEN two things are near enough to interact, and being generous there
	 * costs nothing. Everything that depends on the actual region asks this.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	bool ContainsPoint(FVector WorldPoint) const;

	/** Height of the walkable or swimmable surface. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	float GetSurfaceHeight() const { return GroundHeight + GetSurfaceOffset(); }

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UBoxComponent> Bounds;

	/**
	 * What makes a body react, conduct, wet and ignite.
	 *
	 * A pool does not implement any of those. It carries a volume made of its
	 * own element, and every one of those behaviours already falls out of that:
	 * the reaction subsystem resolves a spell landing in it, conduction floods
	 * lightning across it, ambient exposure wets whoever stands in it. The fluid
	 * system adds a shape on the ground and nothing else.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGElementalVolumeComponent> Volume;

	/**
	 * What you actually see, rebuilt from the ring every time it changes.
	 *
	 * A DYNAMIC MESH RATHER THAN THE WATER PLUGIN, and the difference is not a
	 * preference. Unreal's water bodies are spline-authored level geometry served
	 * by a water zone -- right for a river someone placed, wrong for a puddle a
	 * spell made half a second ago and whose outline changes four times a second.
	 * A body here is already a polygon, so drawing it is a triangulation and an
	 * extrude, and the water LOOK is entirely the material's job.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UDynamicMeshComponent> Surface;

	/** Ground height the body formed at. */
	UPROPERTY(ReplicatedUsing = OnRep_Body, BlueprintReadOnly, Category = "ARPG|Fluid")
	float GroundHeight = 0.f;

	/** Triangles in the drawn surface. Zero when there is nothing to see. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	int32 GetSurfaceTriangleCount() const;

	/**
	 * The drawn surface as a plain primitive, for anything that only wants its
	 * collision or its bounds.
	 *
	 * Here so a caller does not have to depend on GeometryFramework to ask
	 * whether the floe is walkable -- see the module's own note on why the mesh
	 * type stays private to ARPGWorld.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	UPrimitiveComponent* GetSurfaceComponent() const;

	/** What the surface is drawn with, or null if nothing was applied. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	UMaterialInterface* GetSurfaceMaterial() const;

protected:
	/** Rebuilds the trigger box, the volume, the mesh and the collision. */
	virtual void RebuildFromRing();

	/** How far the surface sits above the ground. */
	virtual float GetSurfaceOffset() const { return 0.f; }

	/** Below this area the body is not worth keeping. From its definition. */
	virtual double GetMinimumArea() const { return 0.0; }

	/** The definition's material, loaded. Null for a body with nothing authored. */
	virtual UMaterialInterface* ResolveSurfaceMaterial() const { return nullptr; }

	/** Interior gaps to cut out of the mesh. Only a solid has any. */
	virtual const TArray<FVector2D>& GetMeshHole() const;

	/**
	 * Every replicated field lands here, and every one of them rebuilds.
	 *
	 * ONE NOTIFY FOR ALL OF THEM on purpose. The ring, the ground height and the
	 * definition arrive in no guaranteed order, and a rebuild driven by whichever
	 * came first would size the mesh with a null definition and leave it wrong.
	 * Rebuilding on each is idempotent and lands on the right answer whenever the
	 * last one turns up.
	 */
	UFUNCTION()
	void OnRep_Body();

	UPROPERTY(ReplicatedUsing = OnRep_Body)
	TArray<FVector2D> Ring;
};

/**
 * One persistent body of fluid -- a puddle of water, a pool of lava. Port of
 * Godot's FluidPool.
 */
UCLASS()
class ARPGWORLD_API AARPGFluidPool : public AARPGFluidBody
{
	GENERATED_BODY()

public:
	UPROPERTY(ReplicatedUsing = OnRep_Body, BlueprintReadOnly, Category = "ARPG|Fluid")
	TObjectPtr<UARPGFluidDefinition> Definition;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** A lake is bottomless, so freezing and boiling both take nothing from it. */
	virtual bool ConsumeSurfaceArea(double Area) override;
	virtual float GetSurfaceEnergyDensity() const override;

	/** Configures the pool. Called by the subsystem; nothing else builds one. */
	void Setup(UARPGFluidDefinition* InDefinition, const TArray<FVector2D>& InRing,
		float InGroundHeight);

protected:
	virtual void RebuildFromRing() override;
	virtual float GetSurfaceOffset() const override;
	virtual double GetMinimumArea() const override;
	virtual UMaterialInterface* ResolveSurfaceMaterial() const override;
};

/**
 * A slab frozen out of a fluid -- an ice floe, a crust of obsidian. Port of
 * Godot's FluidSolid.
 *
 * KEEPS ITS HOLES, unlike a pool. A liquid flows back over a hole cut in it; a
 * solid does not, and a floe with a melted-through gap is a floe with a gap you
 * can fall into. See ARPGFluidGeometry::IntersectWithHoles.
 */
UCLASS()
class ARPGWORLD_API AARPGFluidSolid : public AARPGFluidBody
{
	GENERATED_BODY()

public:
	UPROPERTY(ReplicatedUsing = OnRep_Body, BlueprintReadOnly, Category = "ARPG|Fluid")
	TObjectPtr<UARPGSolidDefinition> Definition;

	/** Interior gaps, kept separate from the outline -- see the class comment. */
	UPROPERTY(ReplicatedUsing = OnRep_Body, BlueprintReadOnly, Category = "ARPG|Fluid")
	TArray<FVector2D> HoleRing;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	void Setup(UARPGSolidDefinition* InDefinition, const TArray<FVector2D>& InRing,
		float InGroundHeight);

	/** Inside the outline AND not inside a hole. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	bool IsStandableAt(FVector WorldPoint) const;

	virtual float GetSurfaceEnergyDensity() const override;

protected:
	virtual void RebuildFromRing() override;
	virtual float GetSurfaceOffset() const override;
	virtual double GetMinimumArea() const override;
	virtual UMaterialInterface* ResolveSurfaceMaterial() const override;
	virtual const TArray<FVector2D>& GetMeshHole() const override { return HoleRing; }
};
