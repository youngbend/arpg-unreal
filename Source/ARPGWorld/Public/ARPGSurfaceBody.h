// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ARPGElementalReactive.h"
#include "ARPGElementalSurface.h"
#include "ARPGFluidGeometry.h"
#include "ARPGSurfaceBody.generated.h"

class UARPGElementalVolumeComponent;
class UARPGFluidDefinition;
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
class ARPGWORLD_API AARPGSurfaceBody : public AActor, public IARPGElementalSurface,
	public IARPGElementalReactive
{
	GENERATED_BODY()

public:
	AARPGSurfaceBody();

	//~ AActor
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor

	/** The tag every surface body registers under with USignificanceManager. */
	static const FName SignificanceTag;

	/**
	 * How much this body matters right now: 1 underfoot, 0 beyond reach.
	 *
	 * REGISTERED WITH THE ENGINE rather than computed on demand. The fluid
	 * subsystem already read USignificanceManager for its VIEWPOINTS, then threw
	 * that away and did its own linear distance scan per body -- and the budget
	 * cull, which is the one place the answer actually changes what happens,
	 * ignored significance entirely and dropped whichever body had the smallest
	 * area. A puddle at the player's feet is small; the field they crossed ten
	 * minutes ago is large. The cull was reliably picking the wrong one.
	 *
	 * Registering means the engine maintains this against its own viewpoint list,
	 * on its own schedule, and every other system that wants to know what matters
	 * reads the same number.
	 */
	float GetSignificance() const;

	//~ IARPGElementalSurface. A body is small enough to hand back whole, so the
	// contact bound is ignored -- the caller clips against the agent anyway.
	virtual TArray<FVector2D> GetSurfaceFootprint(const FVector2D& Centre, double Radius) const override;
	virtual float GetSurfaceLevelAt(const FVector2D& At) const override;
	virtual bool ConsumeSurfaceArea(double Area) override;

	/** NO by default -- a slab of ice is not something you can pour water into. */
	virtual bool AbsorbSurfaceVolume(double InVolume) override { return false; }

	virtual float GetSurfaceEnergyDensity() const override { return 0.f; }
	virtual bool IsSurfaceAt(const FVector2D& At) const override;
	virtual FVector2D GetSurfaceFlowAt(const FVector2D& At) const override { return FVector2D::ZeroVector; }
	virtual float GetSurfaceDensity() const override { return 0.f; }
	virtual float GetSurfaceBedAt(const FVector2D& At) const override { return GroundHeight; }
	//~ End IARPGElementalSurface

	/**
	 * Slides the outline without touching the mesh.
	 *
	 * A TRANSLATION CHANGES NO LOCAL GEOMETRY, so retriangulating and recooking
	 * collision for one would be work for a shape that did not change -- and it is
	 * what makes a floe drifting on a current affordable every frame rather than
	 * four times a second. It also leaves the surface texture pinned to the body
	 * rather than to the world, which for something drifting is the right answer
	 * and for something eroding in place is not; see BuildSlabMesh's UVs.
	 */
	void TranslateRing(const FVector2D& Delta);

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
	virtual double GetArea() const;

	/**
	 * Will this body ever go away on its own?
	 *
	 * False for everything that erodes -- a puddle evaporates, a floe melts -- and
	 * true for a slab whose definition says it never does. What asks is the body
	 * budget: culling litter is unnoticeable, culling a wall a player raised for
	 * cover is the worst thing that economy could do.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	virtual bool IsPermanent() const { return false; }

	/**
	 * Is this world point genuinely inside?
	 *
	 * THE COLLIDER IS NOT THE BODY. The trigger box is a rectangle while the
	 * body is an arbitrary polygon -- deliberately, because the box only decides
	 * WHEN two things are near enough to interact, and being generous there
	 * costs nothing. Everything that depends on the actual region asks this.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	virtual bool ContainsPoint(FVector WorldPoint) const;

	/** Height of the walkable or swimmable surface, including how far it is riding. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	float GetSurfaceHeight() const { return GroundHeight + GetVerticalOffset() + GetSurfaceOffset(); }

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

	/**
	 * How far the whole body is displaced from where its outline says it sits.
	 *
	 * Zero for anything resting on the ground, which is every fluid: a puddle IS
	 * its ground height. A floe rides, so it answers with its draft -- and that is
	 * the only axis the polygon model cannot express, which is why it is a
	 * separate number rather than something smuggled into the ring.
	 */
	virtual float GetVerticalOffset() const { return 0.f; }

	/** The definition's material, loaded. Null for a body with nothing authored. */
	virtual UMaterialInterface* ResolveSurfaceMaterial() const { return nullptr; }

	/** Interior gaps to cut out of the mesh. Only a solid has any. */
	virtual const TArray<FVector2D>& GetMeshHole() const;

	/**
	 * The floor this body is lying on, or unset for one that is simply flat.
	 *
	 * A FLOE DOES NOT WANT THIS and a puddle does, which is the whole reason it
	 * is virtual rather than a field. A floe rides a surface -- it is level by
	 * definition, whatever is under it -- while a puddle is poured onto the
	 * ground and takes the shape of it.
	 */
	virtual ARPGFluidGeometry::FBedSampler GetBedSampler() const { return {}; }

	/**
	 * How finely the cap is subdivided so it has somewhere to bend. Zero for a
	 * flat body, which needs no interior vertices at all.
	 */
	virtual double GetBedDetailSpacing() const { return 0.0; }

	/** How far the floor under this body rises and falls, for its bounds. */
	virtual float GetBedSpan() const { return 0.f; }

	/**
	 * How far the body's underside is sunk BELOW the floor it lies on.
	 *
	 * Zero for anything whose underside is genuinely visible -- a floe has water
	 * under it and you can swim beneath the edge of one. A puddle does not: its
	 * underside is pressed against the ground, and drawing the two in the same
	 * plane is asking the depth buffer to choose between them. It cannot, so it
	 * chooses differently per pixel and per frame, which is the shimmer.
	 */
	virtual float GetUndersideSink() const { return 0.f; }

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
class ARPGWORLD_API AARPGFluidPool : public AARPGSurfaceBody
{
	GENERATED_BODY()

public:
	UPROPERTY(ReplicatedUsing = OnRep_Body, BlueprintReadOnly, Category = "ARPG|Fluid")
	TObjectPtr<UARPGFluidDefinition> Definition;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** A lake is bottomless, so freezing and boiling both take nothing from it. */
	virtual bool ConsumeSurfaceArea(double Area) override;

	/**
	 * YES -- the outline grows to hold it. Consumption run backwards.
	 *
	 * NOT A MERGED CIRCLE, which is the version that looks right and silently
	 * loses the water: a circle landing inside an outline unions to that same
	 * outline, so meltwater returned to the middle of a puddle would vanish. The
	 * volume has nowhere to go but the ring.
	 */
	virtual bool AbsorbSurfaceVolume(double InVolume) override;

	virtual float GetSurfaceEnergyDensity() const override;
	virtual float GetSurfaceDensity() const override;

	/** Configures the pool. Called by the subsystem; nothing else builds one. */
	void Setup(UARPGFluidDefinition* InDefinition, const TArray<FVector2D>& InRing,
		float InGroundHeight);

	/**
	 * Height of the floor under a world XY, relative to GroundHeight.
	 *
	 * Zero on the level ground the pool was laid at, negative downhill of it.
	 * Interpolated between samples, so it is continuous rather than stepped.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	float GetBedOffsetAt(const FVector2D& At) const;

	//~ A pool follows its floor, so neither of these is one number any more.
	virtual float GetSurfaceLevelAt(const FVector2D& At) const override;
	virtual float GetSurfaceBedAt(const FVector2D& At) const override;

protected:
	virtual void RebuildFromRing() override;
	virtual float GetSurfaceOffset() const override;
	virtual double GetMinimumArea() const override;
	virtual UMaterialInterface* ResolveSurfaceMaterial() const override;

	virtual ARPGFluidGeometry::FBedSampler GetBedSampler() const override;
	virtual double GetBedDetailSpacing() const override;

	/**
	 * Buried, because the ground is right there.
	 *
	 * Has to clear the error between the floor the mesh is built from -- sampled
	 * on a grid and interpolated between -- and the floor actually drawn, or the
	 * underside weaves above and below it and shimmers along the seam.
	 */
	virtual float GetUndersideSink() const override { return 4.f; }
	virtual float GetBedSpan() const override { return BedSpan; }

	/**
	 * Traces the floor under the outline, into a grid this keeps.
	 *
	 * NOT REPLICATED, AND DELIBERATELY NOT. The floor is level geometry: every
	 * machine already has it, identical, and sampling it locally costs a few
	 * traces where sending it would cost a heightfield per puddle per change. It
	 * is the same argument the ring itself is replicated on -- send what cannot
	 * be derived, derive the rest -- and it means server and client agree by
	 * construction rather than by hoping the numbers survived quantisation.
	 *
	 * CACHED ACROSS REBUILDS. A pool is rebuilt every time weather erodes it,
	 * which is four times a second, and the floor underneath has not moved: only
	 * cells nobody has sampled yet cost a trace, so an evaporating puddle -- which
	 * only ever shrinks -- pays for its grid once.
	 */
	void SampleBed();

	/** Grid of floor heights relative to GroundHeight. Empty until sampled. */
	UPROPERTY(Transient)
	TArray<float> BedSamples;

	/** Which of those have actually been traced for. */
	UPROPERTY(Transient)
	TArray<uint8> BedKnown;

	UPROPERTY(Transient)
	FVector2D BedOrigin = FVector2D::ZeroVector;

	UPROPERTY(Transient)
	float BedSpacing = 0.f;

	UPROPERTY(Transient)
	int32 BedCountX = 0;

	UPROPERTY(Transient)
	int32 BedCountY = 0;

	/** The deepest the floor gets under this pool, for the trigger box.  */
	UPROPERTY(Transient)
	float BedSpan = 0.f;
};
