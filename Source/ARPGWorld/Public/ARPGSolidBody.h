// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGSurfaceBody.h"
#include "ARPGSolidField.h"
#include "ARPGWallRun.h"
#include "ARPGSolidBody.generated.h"

class UARPGSolidDefinition;

/**
 * A slab frozen out of a fluid -- an ice floe, a crust of obsidian. Port of
 * Godot's FluidSolid.
 *
 * KEEPS ITS HOLES, unlike a pool. A liquid flows back over a hole cut in it; a
 * solid does not, and a floe with a melted-through gap is a floe with a gap you
 * can fall into. See ARPGFluidGeometry::IntersectWithHoles.
 */
UCLASS()
class ARPGWORLD_API AARPGSolidBody : public AARPGSurfaceBody
{
	GENERATED_BODY()

public:
	AARPGSolidBody();

	virtual void Tick(float DeltaTime) override;

protected:
	/**
	 * Climbs a rooted slab out of the ground. Nothing else moves it.
	 *
	 * Separate from the buoyancy path rather than a branch inside it: a floe is
	 * being pushed around by a surface every frame forever, and a raised slab is
	 * doing one thing once and then standing still.
	 */
	void Rise(float DeltaTime);

	/**
	 * Turns a drifting slab with the shear in the current under it.
	 *
	 * @param Centre world XY of the slab. @param Flow the current carrying it.
	 */
	void Spin(float DeltaTime, const FVector2D& Centre, const FVector2D& Flow);

public:

	UPROPERTY(ReplicatedUsing = OnRep_Body, BlueprintReadOnly, Category = "ARPG|Fluid")
	TObjectPtr<UARPGSolidDefinition> Definition;

	/**
	 * The body of water this froze out of, and therefore the one it rides.
	 *
	 * A floe is not an independent object: it asks this for the waterline under
	 * it, for the current carrying it, and for where the water stops. Null means
	 * it is aground -- the water went and the ice did not, which the subsystem
	 * treats as the end of it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Fluid")
	TScriptInterface<IARPGElementalSurface> FloatsOn;

	/**
	 * Wedged against the shore, and so going nowhere.
	 *
	 * A spell that freezes the entire width of a river makes a PLUG, not a raft:
	 * it is braced on both banks and the current cannot take it. Recomputed as the
	 * floe melts, so one that has narrowed enough to come free does.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Fluid")
	bool bAnchored = false;

	/**
	 * Too heavy for what it formed on, so it is resting on the bed rather than
	 * riding. Not a failure -- a crust denser than its own fluid sinks, and the
	 * same equation says so.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Fluid")
	bool bAground = false;

	/** How deep it is riding, in cm below the waterline. Replicated as the result. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "ARPG|Fluid")
	float Draft = 0.f;

	/** Things currently standing on it, from the last settle. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	int32 GetOccupantCount() const { return OccupantCount; }

	/**
	 * Recomputes whether the shore has hold of it.
	 *
	 * Not per frame: it changes only as the floe melts or drifts, and it costs a
	 * containment probe per direction. The subsystem's weather tick is where it
	 * happens, beside the melting that is the main reason it would change.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	void UpdateAnchoring();

	/**
	 * WHAT THE SLAB ACTUALLY IS. See FARPGSolidField.
	 *
	 * The outline it froze with was only the seed; from then on the ice is a
	 * heightfield, because everything interesting that happens to a floe happens
	 * in the third dimension -- a bowl melted at an angle into one edge, a step
	 * where new ice formed at the waterline a load had pushed the surface down to,
	 * a hole where the top met the bottom.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Body, BlueprintReadOnly, Category = "ARPG|Fluid")
	FARPGSolidField Field;

	/**
	 * Melts a bowl into the slab at a world point, deepest at the centre.
	 *
	 * WHERE IT WAS HIT, which is the whole difference between ice and a puddle: a
	 * liquid loses ground uniformly because it has no third dimension to lose it
	 * in, and a slab does. At the edge the bowl runs off the side and leaves an
	 * angled cut; in the middle, deep enough, it opens a hole.
	 *
	 * @return volume of ice removed, in cubic cm.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	double MeltAt(const FVector2D& Where, float Radius, float Depth);

	/** Thins the slab from both faces at once. Ambient warmth, and holes widen free. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	double MeltUniformly(float FromTop, float FromBottom);

	/**
	 * Pulls the slab's outline in, taking the rim with it. See FARPGSolidField::Erode.
	 *
	 * The other half of ambient melting, and the half that makes a floe SHRINK
	 * rather than only thin. Without it every cell reaches zero thickness on the
	 * same tick and the whole sheet vanishes between two frames.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	double MeltInward(float Distance);

	/**
	 * Takes the slab off the water and stands it on the bed.
	 *
	 * WHAT A FROZEN-SOLID PUDDLE IS. Freezing the last of a pool leaves ice where
	 * the water was and no water under it, so the slab is not floating on anything
	 * -- and a floe placed at the WATERLINE with no buoyancy tick to settle it
	 * hangs in the air by exactly the depth the puddle had. GroundHeight tracks
	 * the surface a slab rides; for one that rides nothing it has to be the floor.
	 *
	 * Null FloatsOn is how this codebase says rooted, so this is the same state a
	 * raised earth wall is in -- see AARPGRaiseSlabEffect.
	 */
	void GroundOnBed(float BedHeight);

	/**
	 * Puts a melted volume of this slab back into the world as fluid.
	 *
	 * REACTIONS ONLY. Ambient melting calls MeltUniformly directly and discards
	 * what it removed, on purpose: a floe thinning in the sun should leave the
	 * world no wetter than it found it. Fire through ice is the case that leaves
	 * water, because it happened somewhere the player was looking.
	 *
	 * @param MeltedVolume cubic cm of SLAB. Converted by the two densities.
	 * @param At world XY where it melted, so the water appears at the hole.
	 */
	void ReturnMeltedFluid(double MeltedVolume, const FVector2D& At);

	// --- Runoff -----------------------------------------------------------------

	/**
	 * Runs the film over the slab and puts down whatever came off the edge.
	 *
	 * TICKED WHEREVER THE SLAB IS, floating or rooted, because water running off a
	 * floe is the same water running off a tower. Costs nothing at all when the
	 * slab is dry, which is nearly always -- HasWet is a cached read.
	 */
	void TickRunoff(float DeltaTime);

	/** Height of whatever the slab is standing on or floating in. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	float GroundBelow() const;

	/** Fluid currently on the slab, in cubic cm. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	double GetFilmVolume() const { return Field.WetVolume(); }

	/**
	 * Runoff held back because it was not yet worth a body, in cubic cm.
	 *
	 * The film sheds in dribbles by design and every deposit is a polygon merge or
	 * an actor spawn, so it is batched. Visible for the tests, which would
	 * otherwise have to infer it from a puddle that has not appeared yet.
	 */
	/**
	 * Everything this slab has melted that is not yet in a pool.
	 *
	 * THE BATCH AND THE WALL BOTH, because "pending" means the world has not got
	 * it yet -- and volume held in a run creeping down the face is exactly as
	 * undelivered as volume waiting to be worth a deposit. Reporting only the
	 * batch would let a test watch the whole journey and conclude nothing was in
	 * flight.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	double GetPendingRunoff() const;

	/** How much is on the face right now, on its way down. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	double GetWallVolume() const;

	/** The runs currently on the outside of the slab. */
	const TArray<FARPGWallRun>& GetWallRuns() const { return WallRuns; }

	/**
	 * Where the last reaction touched this slab, in world XY.
	 *
	 * THE HOOK CANNOT CARRY A POSITION. OnElementalReaction reports how much
	 * energy was spent and nothing about where, which is fine for a puddle -- a
	 * liquid loses ground uniformly wherever it was hit -- and is the entire
	 * difference for a slab, because melting WHERE the fireball landed is the
	 * behaviour. The reaction solver knows the contact point and leaves it here on
	 * its way past; the hook takes it and clears it.
	 */
	void NoteContactAt(const FVector2D& Where) { PendingContact = Where; bHasPendingContact = true; }

	// --- Where the field is, and which way round --------------------------------
	//
	// THE FIELD IS NOT IN WORLD SPACE, and until now it accidentally was. Its
	// cells were addressed by world XY, which worked only because the actor's
	// rotation was always identity -- a latent coupling rather than a decision,
	// and the one thing standing between a floe and turning as it drifts.
	//
	// A HEIGHTFIELD CAN YAW PERFECTLY WELL. Columns run along world Z, so spinning
	// about the up axis leaves every one of them vertical; it is pitch and roll
	// that a heightfield cannot survive. A floe spins on the water, it does not
	// tumble, so yaw is exactly the rotation this shape can afford -- see
	// AARPGLaunchSlabProjectile for the case that genuinely needs tumbling.
	//
	// The mesh was always built in field space relative to the centroid, so it
	// needed no change at all: the component's own transform carries the yaw.

	/** World XY of the field's own coordinate origin. */
	UPROPERTY(ReplicatedUsing = OnRep_Body, BlueprintReadOnly, Category = "ARPG|Fluid")
	FVector2D FieldOrigin = FVector2D::ZeroVector;

	/** Degrees the field is turned by, about the up axis. */
	UPROPERTY(ReplicatedUsing = OnRep_Body, BlueprintReadOnly, Category = "ARPG|Fluid")
	float FieldYaw = 0.f;

	/** World XY of a point in the field's frame. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	FVector2D ToWorld(const FVector2D& Local) const
	{
		return FieldOrigin + Local.GetRotated(FieldYaw);
	}

	/** The field's frame, from a world XY. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	FVector2D ToField(const FVector2D& World) const
	{
		return (World - FieldOrigin).GetRotated(-FieldYaw);
	}

	/** A DIRECTION into the field's frame -- turned, not moved. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	FVector2D DirToField(const FVector2D& World) const { return World.GetRotated(-FieldYaw); }

	/** World XY of whatever is left of the slab. What the actor sits at. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	FVector2D GetWorldCentre() const { return ToWorld(Field.SolidCentroid()); }

	/**
	 * Gap between a world point and the slab's edge, or 0 inside it.
	 *
	 * TO THE SLAB, NOT TO ITS ORIGIN. A wall is long and its origin is its
	 * centroid, so measuring to the actor would tell a caster standing at one end
	 * of one that they were nowhere near it. Here rather than in the subsystem so
	 * the frame conversion has exactly one home.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	double DistanceToEdge(const FVector2D& World) const;

	/** Thins the whole slab, and reports when there is too little left to be one. */
	virtual bool ConsumeSurfaceArea(double Area) override;

	/**
	 * Is there slab at this point?
	 *
	 * THE FIELD, NOT THE RING. A slab's shape is its cells: Setup seeds them from
	 * an outline and everything afterwards -- a melt, a refreeze, a hole through
	 * it -- happens to the cells, with no polygon kept in step. The base class
	 * answers this from Ring, which for a slab is empty, so every point on every
	 * slab read as outside and FindSolidAt never found one.
	 *
	 * Not the same question as IsStandableAt, which also asks whether the
	 * definition means the slab to be walked on at all.
	 */
	virtual bool ContainsPoint(FVector WorldPoint) const override;

	virtual double GetArea() const override;

	/** Nothing takes it with time, so nothing should take it for room either. */
	virtual bool IsPermanent() const override;

	/**
	 * Starts the slab buried and lets it climb out. What a spell that RAISES one
	 * calls instead of leaving it standing there from the first frame.
	 *
	 * Depth is how far under its resting height to begin -- its own thickness
	 * hides it completely. Costs nothing after it arrives.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	void BeginBuried(float Depth);

	/**
	 * Adopts a field wholesale, rather than building one from an outline.
	 *
	 * WHAT LANDS IS WHAT WAS THROWN. Setup seeds a field from a ring, which is
	 * right when a slab is being made and wrong when one is being put back: a
	 * pillar that was carved by two fireballs before it was picked up should come
	 * down carved. This is the other door into the same object.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	void AdoptField(UARPGSolidDefinition* InDefinition, const FARPGSolidField& InField,
		const FVector& Where, float Yaw);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	void Setup(UARPGSolidDefinition* InDefinition, const TArray<FVector2D>& InRing,
		float InGroundHeight);

	/** Inside the outline AND not inside a hole. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	bool IsStandableAt(FVector WorldPoint) const;

	virtual float GetSurfaceEnergyDensity() const override;

	/**
	 * The top of the slab HERE, which after a spell has landed on it is not one
	 * number: a bowl melted into the middle is centimetres lower than the rim
	 * around it, and the field has said so all along.
	 */
	virtual float GetSurfaceLevelAt(const FVector2D& At) const override;

	/**
	 * What has melted and is lying on top of the rock, drawn as its own surface.
	 *
	 * SEPARATE FROM Surface, not a second material on it, for two reasons that
	 * both come down to the film being a different KIND of thing. It is drawn
	 * with the FLUID's material rather than the solid's -- lava, not the earth it
	 * is running down -- and it carries no collision, because what you stand on
	 * is the rock underneath and what the lava does to you is the volume
	 * component's business. It also changes far more often than the rock does:
	 * the film moves every tick while it drains, and the slab it lies on is
	 * rebuilt only when a spell cuts it.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UDynamicMeshComponent> Film;

	/** Redraws the film. Cheap when dry, which is nearly always. */
	void RebuildFilm();

	/** Puts what just left a rim onto the face, to make its own way down. */
	void ShedOntoTheWall(const FVector2D& ShedAt, double Shed);

	/** Advances everything on the face, and banks whatever reached the bottom. */
	void TickWallRuns(float DeltaTime, const UARPGFluidDefinition& Fluid);

	/** How much film is actually being drawn. Zero on rock nobody has melted. */
	int32 GetFilmTriangleCount() const;

	/**
	 * The drawn film as a plain primitive, for anything that only wants its
	 * bounds. Here for the same reason GetSurfaceComponent is -- so a caller
	 * needs no dependency on GeometryFramework to ask where the lava reaches.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	UPrimitiveComponent* GetFilmComponent() const;

protected:
	virtual void RebuildFromRing() override;
	virtual float GetSurfaceOffset() const override;
	virtual double GetMinimumArea() const override;
	virtual float GetVerticalOffset() const override { return -Draft; }
	virtual UMaterialInterface* ResolveSurfaceMaterial() const override;


	/** The depth it WANTS to be riding at, from Archimedes and what is aboard. */
	float ComputeTargetDraft() const;

	/** How many things are standing on the slab right now. */
	int32 CountOccupants() const;

	/** Does the water continue past the floe's edge in this direction? */
	bool HasRoomToward(const FVector2D& Direction) const;

	int32 OccupantCount = 0;

	FVector2D PendingContact = FVector2D::ZeroVector;
	bool bHasPendingContact = false;

	/** Seconds between occupant polls, and the countdown to the next. */
	static constexpr float OccupantPollInterval = 0.25f;

	UPROPERTY(Transient)
	float OccupantPoll = 0.f;

	/**
	 * Everything currently making its way down the outside.
	 *
	 * NOT REPLICATED, on the same rule as the film it came from: this is the
	 * journey, and what matters at the end of it is a pool, which replicates
	 * already. A client sees the lava arrive rather than watching it crawl -- the
	 * cost of sending a per-slab list of runs at tick rate buys a detail nobody
	 * away from the wall can see.
	 */
	UPROPERTY(Transient)
	TArray<FARPGWallRun> WallRuns;

	/** Runoff waiting to be worth a deposit, and where it ran off. */
	UPROPERTY(Transient)
	double PendingRunoff = 0.0;

	UPROPERTY(Transient)
	FVector2D RunoffAt = FVector2D::ZeroVector;

	/**
	 * Ambient melting that has happened but has not been drawn yet, in cm.
	 *
	 * BECAUSE A REBUILD IS THE EXPENSIVE THING A SLAB DOES, and ambient melting
	 * asked for one four times a second forever. A 5m floe at 20cm cells is 2800
	 * cells and roughly 8000 triangles, and rebuilding plus re-cooking collision
	 * measured over 10ms -- a dropped frame, four times a second, per floe. What
	 * it bought was a tenth of a millimetre of thinning: the weather tick moves
	 * the field by MeltRate * 0.25s, which for ice is 1.25mm.
	 *
	 * So the melt lands in the cells every tick -- the simulation never lags the
	 * truth -- and the MESH waits until there is something to see. Anything that
	 * changes the slab's shape rather than its thickness still rebuilds at once;
	 * see MeltUniformly.
	 */
	UPROPERTY(Transient)
	float UndrawnMelt = 0.f;

	/** How much ambient thinning is worth a rebuild, in cm. */
	static constexpr float UndrawnMeltLimit = 0.5f;

};
