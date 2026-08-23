// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGSurfaceBody.h"
#include "ARPGSurfaceFacets.h"
#include "ARPGSolidBody.generated.h"

class UARPGSolidDefinition;

/**
 * A bowl cut into the top of a slab, where something hit it.
 *
 * A SLAB IS NOT A PRISM, and saying it was is the mistake this exists to undo.
 * An outline extruded to a uniform thickness is a choice about how to DRAW the
 * shape, not a limit of what a mesh can hold -- a mesh models a sphere perfectly
 * well, so it models a dish in a slab perfectly well. The upper surface is a
 * height function sampled per vertex; all it took was tessellating the cap and
 * telling it what height to be.
 *
 * A PARABOLOID, deepest at the middle and tapering to nothing at the rim, which
 * is what makes a hit at the edge of a slab cut it away at an angle while the
 * same hit in the middle leaves a bowl. Deep enough and the bowl reaches the
 * underside, at which point the material is genuinely gone and the OUTLINE has to
 * lose it too -- see AARPGSolidBody::BreakAt, which is where the dish stops being
 * a dish and becomes a hole.
 *
 * KEPT AS A LIST rather than baked into the mesh, so the mesh is rebuilt from the
 * outline and these every time and never accumulates. That is the property the
 * heightfield had and repeated boolean editing does not: cost is the base shape
 * plus a bounded number of bowls, however many spells have landed.
 */
USTRUCT(BlueprintType)
struct ARPGWORLD_API FARPGSlabBite
{
	GENERATED_BODY()

	/** Where it landed, in the slab's own frame. */
	UPROPERTY()
	FVector2D At = FVector2D::ZeroVector;

	/** How broad the bowl is, in cm. */
	UPROPERTY()
	float Radius = 0.f;

	/** How deep at the middle, in cm. */
	UPROPERTY()
	float Depth = 0.f;

	/** How far into the slab this bowl reaches at a point, in cm. Zero outside it. */
	float DepthAt(const FVector2D& Where) const
	{
		if (Radius <= 0.f || Depth <= 0.f)
		{
			return 0.f;
		}

		const float DistanceSq = static_cast<float>(FVector2D::DistSquared(Where, At));
		const float RadiusSq = Radius * Radius;

		if (DistanceSq >= RadiusSq)
		{
			return 0.f;
		}

		// The same falloff the heightfield's MeltBowl used, so a hit reads the way
		// it always did: a dish rather than a stamped-out cylinder.
		return Depth * (1.f - DistanceSq / RadiusSq);
	}
};

/**
 * A slab of something solid: an ice floe, an earth wall, a crust of obsidian.
 *
 * AN OUTLINE WITH A THICKNESS, and that is the whole model. The shape is the
 * polygon the slab was made with -- the exact region where a freezing spell
 * overlapped a body of water, or the footprint a spell raised out of the ground
 * -- extruded to a uniform depth. Nothing quantises it, so a floe reproduces the
 * water it froze out of exactly, at whatever resolution the clip produced.
 *
 * THIS REPLACED A HEIGHTFIELD, and the trade is worth stating plainly because
 * the heightfield was not a mistake. A grid of cells could hold things a polygon
 * cannot: a bowl melted into the top at an angle, a hole punched clean through,
 * a step where new ice froze at the waterline a load had pushed the surface down
 * to. Those are real and they are gone. What they cost was a resolution -- every
 * shape rounded to the grid it was stored on, a rebuild and a collision cook
 * whenever anything changed it, and the heaviest payload in the game on the
 * wire. A slab that only ever needs to BE a shape does not need any of that.
 *
 * SO NOTHING MELTS. Ambient warmth no longer takes a slab at all; a floe stays
 * until something breaks it. Breaking is the one thing that still changes a
 * slab's shape, and it is the same operation a puddle already uses -- the
 * outline shrinks about its centre and the body goes when there is too little
 * left to be one. See ConsumeSurfaceArea.
 *
 * THE OUTLINE IS IN THE SLAB'S OWN FRAME, not the world's, which is what lets a
 * floe drift and turn without the mesh being rebuilt: moving it is a transform
 * on the actor and the polygon never learns anything happened. Columns are
 * vertical, so yaw is the only rotation this shape can afford -- a floe spins on
 * the water, it does not tumble.
 */
UCLASS()
class ARPGWORLD_API AARPGSolidBody : public AARPGSurfaceBody
{
	GENERATED_BODY()

public:
	AARPGSolidBody();

	virtual void Tick(float DeltaTime) override;

	UPROPERTY(ReplicatedUsing = OnRep_Body, BlueprintReadOnly, Category = "ARPG|Fluid")
	TObjectPtr<UARPGSolidDefinition> Definition;

	/**
	 * The body of water this froze out of, and therefore the one it rides.
	 *
	 * A floe is not an independent object: it asks this for the waterline under
	 * it, for the current carrying it, and for where the water stops. NULL MEANS
	 * ROOTED -- a wall raised out of the ground has never floated on anything, and
	 * a slab that froze the last of its own puddle is standing on the bed. Neither
	 * settles, drifts, nor asks the ground for a waterline it does not have.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Fluid")
	TScriptInterface<IARPGElementalSurface> FloatsOn;

	/**
	 * Wedged against the shore, and so going nowhere.
	 *
	 * A spell that freezes the entire width of a river makes a PLUG, not a raft:
	 * it is braced on both banks and the current cannot take it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Fluid")
	bool bAnchored = false;

	/**
	 * Resting on the bed rather than riding, because it is too heavy for what it
	 * formed on -- or because the fluid it formed on is gone.
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
	 * Not per frame: it changes only as the floe drifts or is broken, and it costs
	 * a containment probe per direction.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	void UpdateAnchoring();

	// --- Where the slab is, and which way round --------------------------------

	/** World XY of the outline's own coordinate origin. */
	UPROPERTY(ReplicatedUsing = OnRep_Body, BlueprintReadOnly, Category = "ARPG|Fluid")
	FVector2D SlabOrigin = FVector2D::ZeroVector;

	/** Degrees the outline is turned by, about the up axis. */
	UPROPERTY(ReplicatedUsing = OnRep_Body, BlueprintReadOnly, Category = "ARPG|Fluid")
	float SlabYaw = 0.f;

	/** World XY of a point in the slab's frame. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	FVector2D ToWorld(const FVector2D& Local) const
	{
		return SlabOrigin + Local.GetRotated(SlabYaw);
	}

	/** The slab's frame, from a world XY. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	FVector2D ToLocal(const FVector2D& World) const
	{
		return (World - SlabOrigin).GetRotated(-SlabYaw);
	}

	/** A DIRECTION into the slab's frame -- turned, not moved. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	FVector2D DirToLocal(const FVector2D& World) const { return World.GetRotated(-SlabYaw); }

	/** World XY of the middle of the slab. What the actor sits at. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	FVector2D GetWorldCentre() const;

	/**
	 * Gap between a world point and the slab's edge, or 0 inside it.
	 *
	 * TO THE SLAB, NOT TO ITS ORIGIN. A wall is long and its origin is its middle,
	 * so measuring to the actor would tell a caster standing at one end of one
	 * that they were nowhere near it.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	double DistanceToEdge(const FVector2D& World) const;

	/**
	 * Takes ground off the slab, and reports when there is too little left.
	 *
	 * BREAKING IS THE ONLY THING THAT CHANGES A SLAB'S SHAPE now that nothing
	 * melts, and it is the same operation a puddle uses: the outline keeps its
	 * shape and shrinks about its centre. That is exactly right here for a reason
	 * it is only approximately right for a liquid -- a spell does not cut a
	 * region out of a wall and leave a ring standing on nothing; it knocks a wall
	 * down, and a smaller wall is what is left.
	 */
	/**
	 * The ground the slab actually covers: its outline LESS its hole.
	 *
	 * A hit in the middle takes nothing off the outline -- the whole of what it did
	 * shows up as a gap -- so a slab that measured itself by its outline alone would
	 * keep its full area, its full buoyancy and its full footing no matter how much
	 * of the middle had been shot out of it, and could never be worn down to
	 * nothing by hits that all landed inside it.
	 */
	virtual double GetArea() const override;

	virtual bool ConsumeSurfaceArea(double Area) override;

	/**
	 * Cuts the region out of the slab, KEEPING whatever hole that opens.
	 *
	 * The one place a solid and a liquid part company about erosion. A puddle with
	 * a piece taken out of its middle closes over it, so the base class drops the
	 * hole and shrinks instead; a wall with a piece taken out of its middle has a
	 * piece taken out of its middle, and you can see through it.
	 */
	virtual bool ConsumeSurfaceRegion(const TArray<FVector2D>& Region) override;

	/**
	 * Where the last reaction touched this slab, in world XY.
	 *
	 * THE HOOK CANNOT CARRY A POSITION. OnElementalReaction reports how much energy
	 * was spent and nothing about where, which is fine for a puddle -- a liquid
	 * loses ground uniformly wherever it was hit -- and is the entire difference for
	 * a slab, because losing ground WHERE the fireball landed is the behaviour. The
	 * reaction solver knows the contact point and leaves it here on its way past;
	 * the hook takes it and clears it.
	 */
	void NoteContactAt(const FVector2D& Where) { PendingContact = Where; bHasPendingContact = true; }

	virtual void OnElementalReaction_Implementation(float Consumed, float Remaining,
		UARPGMagicElement* Product) override;

	/** Inside the outline, and not inside the hole. */
	virtual bool ContainsPoint(FVector WorldPoint) const override;

	/** The same question, and also whether the definition means it to be walked on. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	bool IsStandableAt(FVector WorldPoint) const;

	/** NOTHING TAKES A SLAB WITH TIME any more, so nothing should take it for room. */
	virtual bool IsPermanent() const override { return true; }

	/** The outline in WORLD space, for anything clipping against this slab. */
	virtual TArray<FVector2D> GetSurfaceFootprint(const FVector2D& Centre,
		double Radius) const override;

	virtual bool IsSurfaceAt(const FVector2D& At) const override;

	virtual float GetSurfaceEnergyDensity() const override;

	/**
	 * Starts the slab buried and lets it climb out. What a spell that RAISES one
	 * calls instead of leaving it standing there from the first frame.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	void BeginBuried(float Depth);

	/**
	 * Takes the slab off the water and stands it on the bed.
	 *
	 * WHAT A FROZEN-SOLID PUDDLE IS. Freezing the last of a pool leaves ice where
	 * the water was and no water under it, so the slab rides nothing -- and one
	 * placed at the WATERLINE with no buoyancy tick to settle it hangs in the air
	 * by exactly the depth the puddle had.
	 */
	void GroundOnBed(float BedHeight);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * Makes the slab, from the outline it is to have.
	 *
	 * The ring is in WORLD space -- it came from clipping one surface against
	 * another -- and is recentred into the slab's own frame here, so everything
	 * afterwards can move the slab without touching the polygon.
	 */
	void Setup(UARPGSolidDefinition* InDefinition, const TArray<FVector2D>& InRing,
		float InGroundHeight, const TArray<FVector2D>& InHole = TArray<FVector2D>());

	/**
	 * Adopts an outline wholesale, keeping the frame it is given.
	 *
	 * WHAT LANDS IS WHAT WAS THROWN. Setup recentres and squares up a fresh slab,
	 * which is right when one is being made and wrong when one is being put back:
	 * a pillar picked up and thrown should come down the shape and the angle it
	 * left as.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	void AdoptOutline(UARPGSolidDefinition* InDefinition, const TArray<FVector2D>& InRing,
		const TArray<FVector2D>& InHole, const FVector& Where, float Yaw);

	/** The outline, in the slab's own frame. */
	const TArray<FVector2D>& GetOutline() const { return Ring; }

	/** The gap through it, if it froze around one or something broke through. */
	const TArray<FVector2D>& GetHole() const { return Hole; }

	/** The bowls cut into its top. */
	const TArray<FARPGSlabBite>& GetBites() const { return Bites; }

	/**
	 * How far the top has been cut down at a world point, in cm.
	 *
	 * Never past the slab's own thickness: below that there is nothing left to
	 * take, and what happens instead is a hole -- see BreakAt.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	float BiteDepthAt(const FVector2D& World) const;

	/**
	 * Cuts a bowl into the slab at a world point, and opens a hole where it goes
	 * clean through.
	 *
	 * @return the plan area the slab lost, which is nothing at all for a dish that
	 *         did not break through.
	 */
	double BreakAt(const FVector2D& Where, float Radius, float Depth);

	/** The top of the slab HERE, which after a spell has landed is not one number. */
	virtual float GetSurfaceLevelAt(const FVector2D& At) const override;

protected:
	virtual void RebuildFromRing() override;
	virtual float GetSurfaceOffset() const override;
	virtual double GetMinimumArea() const override;
	virtual float GetVerticalOffset() const override { return -Draft; }
	virtual UMaterialInterface* ResolveSurfaceMaterial() const override;
	virtual const TArray<FVector2D>& GetMeshHole() const override { return Hole; }
	virtual ARPGFluidGeometry::FBedSampler GetBedSampler() const override;

	/** The bowls, as the height function the mesher cuts the top down by. */
	ARPGFluidGeometry::FBedSampler GetTopRelief() const;
	virtual double GetBedDetailSpacing() const override;

	/**
	 * Climbs a rooted slab out of the ground. Nothing else moves it.
	 *
	 * Separate from the buoyancy path rather than a branch inside it: a floe is
	 * being pushed around by a surface every frame forever, and a raised slab is
	 * doing one thing once and then standing still.
	 */
	void Rise(float DeltaTime);

	/** Turns a drifting slab with the shear in the current under it. */
	void Spin(float DeltaTime, const FVector2D& Centre, const FVector2D& Flow);

	/** The depth it WANTS to be riding at, from Archimedes and what is aboard. */
	float ComputeTargetDraft() const;

	/** How many things are standing on the slab right now. */
	int32 CountOccupants() const;

	/** Does the fluid continue past the slab's edge in this direction? */
	bool HasRoomToward(const FVector2D& Direction) const;

	/** How far the outline reaches from a point along a direction, in its own frame. */
	double SupportDistance(const FVector2D& From, const FVector2D& Direction) const;

	/**
	 * The gap through the slab, in its own frame.
	 *
	 * KEPT, unlike a pool's. A liquid flows back over a hole cut in it; a solid
	 * does not, and a floe with a gap you can fall through is a floe with a gap.
	 * One ring rather than a list because that is what the mesher can draw and
	 * what a clip against a single agent can produce.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Body, BlueprintReadOnly, Category = "ARPG|Fluid")
	TArray<FVector2D> Hole;

	/**
	 * The bowls cut into the top, in the slab's own frame.
	 *
	 * BOUNDED, and that is the whole reason this is a list of a dozen floats
	 * rather than a mesh that has been edited a dozen times. A new hit close to an
	 * old one DEEPENS it instead of appending -- which is also what makes
	 * concentrated fire break through where scattered fire only dishes -- and past
	 * the cap the shallowest is dropped. So the mesh is always the outline plus at
	 * most a few bowls, however long the fight goes on.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Body, BlueprintReadOnly, Category = "ARPG|Fluid")
	TArray<FARPGSlabBite> Bites;

	/** How many bowls a slab is worth drawing. */
	static constexpr int32 MaxBites = 12;

	/**
	 * Puts the VOLUME a spell broke off back into the world as fluid.
	 *
	 * A VOLUME, not an area, and mass is what is conserved across the change. Rock
	 * is denser than the lava it melts into, so a cubic metre of wall is not a cubic
	 * metre of lava -- it is the volume of lava that weighs the same. The two
	 * densities that decide whether a slab floats are the two that decide how much
	 * fluid it is worth, which is the point of them being densities.
	 */
	void ReturnBrokenFluid(double SolidVolume);

	/** Cuts a region out of the outline. The geometry, without the accounting. */
	bool CutRegion(const TArray<FVector2D>& Region);

	/**
	 * How much material a bowl of this shape takes out of a slab this thick.
	 *
	 * A paraboloid removes half the cylinder around it -- until it is deeper than
	 * the slab, at which point the underside truncates it and what is left is
	 * a cylinder with a dimple in the top.
	 */
	static double BowlVolume(float Radius, float Depth, float Thickness);

	int32 OccupantCount = 0;

	FVector2D PendingContact = FVector2D::ZeroVector;
	bool bHasPendingContact = false;

	/** Seconds between occupant polls, and the countdown to the next. */
	static constexpr float OccupantPollInterval = 0.25f;

	UPROPERTY(Transient)
	float OccupantPoll = 0.f;
};
