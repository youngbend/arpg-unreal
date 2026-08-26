// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ARPGElementalReactive.h"
#include "ARPGElementalSurface.h"
#include "ARPGFluidField.h"
#include "ARPGFluidRegion.generated.h"

class UARPGElementalVolumeComponent;
class UARPGFluidDefinition;
class UBoxComponent;
class UDynamicMeshComponent;
class UMaterialInterface;
class UPrimitiveComponent;

/**
 * A HANDLE ON A CONNECTED BODY OF WET CELLS, and nothing else.
 *
 * WHAT IT IS NOT. It is not the water. AARPGFluidPool WAS the water -- its ring
 * was the single source of truth and everything else was rebuilt from it -- and
 * this is the opposite: the field is the truth, this is spawned to stand where a
 * body of it happens to be, and it is destroyed and respawned as puddles merge
 * and split without a drop moving.
 *
 * SO WHY HAVE ONE AT ALL. Because two things in this codebase need water to be an
 * ACTOR and neither of them should have to care that it stopped being one:
 *
 *   AN OVERLAP. UARPGElementalVolumeComponent::OverlapSource wants a real
 *   UPrimitiveComponent with real physics overlaps, because that is how a spell
 *   entering a puddle is noticed at all -- see the static OnVolumesMet delegate.
 *   A subsystem cannot overlap anything.
 *
 *   SOMETHING TO FLOAT ON. AARPGSolidBody::FloatsOn is a
 *   TScriptInterface<IARPGElementalSurface>, and the reaction solver's
 *   anti-cascade guard compares it against both a volume component and that
 *   component's OWNER. Both limbs still work when the owner is this, which is
 *   why freezing a river 620 times over does not come back.
 *
 * PROXIES ARE MATCHED, NOT RESPAWNED, between passes -- see FRegion::AnchorChunk.
 * A floe whose surface was destroyed and rebuilt every quarter second would be a
 * floe that fell into the sea four times a second.
 *
 * NOT REPLICATED. The field is not, so this cannot be: there is nothing to send
 * that a client could not derive, and a proxy is a consequence of the simulation
 * rather than a part of it.
 */
UCLASS()
class ARPGWORLD_API AARPGFluidRegion : public AActor, public IARPGElementalSurface,
	public IARPGElementalReactive
{
	GENERATED_BODY()

public:
	AARPGFluidRegion();

	/** What this is made of. Identity, damage, status and reactions come from it. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ARPG|Fluid")
	TObjectPtr<UARPGFluidDefinition> Definition;

	/**
	 * Points this proxy at a body of cells and rebuilds everything derived.
	 *
	 * THE ONLY WAY THIS ACTOR CHANGES. There is no setter for the outline, the
	 * volume or the bounds, because all three are answers about the field and
	 * anything that could set them independently could make them disagree with it.
	 */
	void UpdateFrom(FARPGFluidField* InField, const FARPGFluidField::FRegion& Region,
		UARPGFluidDefinition* InDefinition);

	//~ IARPGElementalSurface
	//
	// EVERY ONE OF THESE ASKS THE FIELD, which is what makes the seam hold: the
	// interface was already written so that none of its questions is "what class
	// are you", so a body that is a grid rather than a polygon answers all ten
	// without a single caller changing.
	virtual TArray<FVector2D> GetSurfaceFootprint(const FVector2D& Centre, double Radius) const override;
	virtual float GetSurfaceLevelAt(const FVector2D& At) const override;
	virtual bool ConsumeSurfaceArea(double Area) override;
	virtual bool ConsumeSurfaceRegion(const TArray<FVector2D>& Region) override;

	/**
	 * YES, and it is the first honest yes this interface has had.
	 *
	 * The pool's comment called false "the ordinary answer and not a failure",
	 * because a puddle would rather the water were deposited where it appeared
	 * than have its outline grow uniformly somewhere the ice never was. A field
	 * puts it exactly where it appeared, so the reason for saying no is gone.
	 */
	virtual bool AbsorbSurfaceVolume(double InVolume) override;

	virtual float GetSurfaceEnergyDensity() const override;
	virtual bool IsSurfaceAt(const FVector2D& At) const override;
	virtual FVector2D GetSurfaceFlowAt(const FVector2D& At) const override;
	virtual float GetSurfaceDensity() const override;
	virtual float GetSurfaceBedAt(const FVector2D& At) const override;
	//~ End IARPGElementalSurface

	/**
	 * A REACTION TAKES GROUND, NOT SCALE -- the same argument AARPGSurfaceBody
	 * makes, and it has to be made again here because this shares no base with it.
	 * Without the override a puddle gets the default projectile reaction: scaled
	 * down by the cube root of what is left, and destroyed outright once spent.
	 */
	virtual void OnElementalReaction_Implementation(float Consumed, float Remaining,
		UARPGMagicElement* Product) override;

	/**
	 * Takes every drop of this body out of the field.
	 *
	 * WHAT CULLING A BODY OF WATER MEANS NOW. A pool was an actor and culling it
	 * was destroying the actor; a proxy is a handle, and destroying one would
	 * leave the water exactly where it was with nothing standing for it -- so the
	 * budget would appear to work and the field would go on holding every drop.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Fluid")
	void Drain();

	/**
	 * Is there too little left here to go on being a body?
	 *
	 * THE SAME TEST RECONCILEREGIONS APPLIES, deliberately, so a body cannot
	 * report itself spent to the reaction that emptied it and then be found again
	 * on the next pass.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	bool IsSpent() const;

	/** Is this world point genuinely in the water, rather than merely in the box? */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	bool ContainsPoint(FVector WorldPoint) const;

	/** The body's outline, traced from its cells. Empty until the first update. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	const TArray<FVector2D>& GetRing() const { return Ring; }

	/**
	 * Is a Niagara sheet drawing this body?
	 *
	 * PER BODY, NOT PER WORLD. The stopgap slab used to hide whenever presentation
	 * was switched on at all, which was right while there was one sheet following
	 * the viewer -- now there is one sheet per body and only as many as the budget
	 * allows, so a puddle beyond the budget has to keep drawing its own slab or it
	 * vanishes. Set by UARPGFluidPresentationSubsystem, which is the only thing
	 * that knows which bodies it took.
	 */
	void SetDrawnBySheet(bool bDrawn);

	/** Ground the body covers, in square cm. The CELLS, not the ring -- see below. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	double GetArea() const { return Area; }

	/** Every drop in it, in cubic cm. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	double GetVolume() const { return WaterVolume; }

	/** Height of the surface at the body's middle. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	float GetSurfaceHeight() const { return MaxLevel; }

	/**
	 * How high the floor is under the deepest part of the body.
	 *
	 * WHERE THE ACTOR SITS, and not the waterline: a body running down a ramp has
	 * one bed per cell and this is the lowest of them. Anything that wants the
	 * floor at a PARTICULAR point should ask GetSurfaceBedAt, which asks the field.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	float GetGroundHeight() const { return MinBed; }

	/** Triangles in the stopgap surface. Zero when there is nothing to see. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	int32 GetSurfaceTriangleCount() const;

	/**
	 * The drawn surface as a plain primitive, for anything that only wants its
	 * bounds. Here so a caller does not have to depend on GeometryFramework.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	UPrimitiveComponent* GetSurfaceComponent() const;

	/** What the surface is drawn with, or null if nothing was applied. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	UMaterialInterface* GetSurfaceMaterial() const;

	/**
	 * A scatter of cells this body held, for matching it to itself next pass.
	 *
	 * NOT ONE ANCHOR, and the difference is a bug rather than a refinement. A
	 * single cell nearest the middle is exactly the cell a spell freezing the
	 * centre of a puddle takes -- so the proxy failed to match the body it was
	 * still standing in, was retired, and took the floe that had just been frozen
	 * out of it down with it. A scatter survives losing any part of the body that
	 * is not all of it.
	 *
	 * Matching is by how MUCH they share, so when a puddle splits in two the
	 * larger half keeps the actor and whatever was floating on it.
	 */
	const TSet<TPair<FIntPoint, int32>>& GetCells() const { return Cells; }

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UBoxComponent> Bounds;

	/**
	 * What makes a body react, conduct, wet and ignite.
	 *
	 * A region implements none of those. It carries a volume made of its element,
	 * and every one of those behaviours already falls out of that -- the same
	 * division AARPGSurfaceBody documents, kept exactly as it was so nothing
	 * downstream can tell which kind of body it is talking to.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UARPGElementalVolumeComponent> Volume;

	/**
	 * A STOPGAP, and named as one.
	 *
	 * The finished surface is a Niagara shallow-water sheet reading the field as a
	 * texture, which is a milestone away. Until then this draws the traced outline
	 * as a flat slab so that water is still something you can see -- blocky,
	 * because the ring follows cell edges exactly and smoothing it would make the
	 * drawn shape disagree with the one that gets frozen.
	 *
	 * Delete this component when the sheet lands. Nothing queries it.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UDynamicMeshComponent> Surface;

protected:
	/** Is the cell under this world XY one of ours? */
	bool OwnsCellAt(const FVector2D& At) const;

	/** How far a consume may reach from the middle when taking water off. */
	float GetReach() const;

	/**
	 * Re-reads how much water this body has, without waiting for a reconcile.
	 *
	 * A REACTION HAPPENS BETWEEN PASSES. TrySolidify freezes part of a puddle and
	 * then asks the same proxy how much ground it has left, all inside one call --
	 * and a proxy whose numbers were last refreshed a quarter of a second ago
	 * answers with the area it had BEFORE the ice took any, so the freeze reads as
	 * having cost the water nothing.
	 *
	 * Bounded by the body's own reach rather than recomputed from a stored cell
	 * list, which is what keeps a proxy a handle rather than a second copy of the
	 * field.
	 */
	void RefreshMetrics();

	UPROPERTY(Transient)
	TArray<FVector2D> Ring;

	/** See SetDrawnBySheet. */
	bool bDrawnBySheet = false;

	/**
	 * THE CELLS, NOT THE RING, and the two genuinely differ.
	 *
	 * Water round a pillar has an outline that encloses the pillar, because a
	 * fluid keeps no hole -- so the ring is larger than the ground the water is
	 * actually on. Everything that meters the body (how much energy it carries,
	 * whether it counts as a reservoir, how much a reaction can take) has to use
	 * the smaller number, or a puddle gets credit for ground it is not on.
	 */
	double Area = 0.0;

	/**
	 * Which floor this body is on. See FARPGFluidField::FRegion::Layer.
	 *
	 * THE PROXY IS WHERE THE LAYER LIVES, and that is the whole reason a proxy
	 * still exists at all now that the field holds the water. Every question
	 * IARPGElementalSurface asks takes an FVector2D -- there is no Z anywhere in
	 * the interface to say whether the caller means the balcony or the room under
	 * it -- so the answer has to come from the body being asked rather than from
	 * the question.
	 */
	int32 Layer = 0;

	double WaterVolume = 0.0;
	float MaxLevel = 0.f;
	float MinBed = 0.f;
	FVector2D Centroid = FVector2D::ZeroVector;

	/**
	 * The cells this body is made of.
	 *
	 * KEPT, THOUGH IT IS A COPY, and the reason is that without it a proxy cannot
	 * tell its own water from anybody else's. Every body of one element shares one
	 * field, so "is there water at this point" is a question about the WORLD, and
	 * a region answering IsSurfaceAt from the field alone claims every puddle in
	 * it -- which made a deposit five hundred metres away merge into a body it
	 * could not reach, and a chain of three separate puddles read as one.
	 *
	 * THE COST IS BOUNDED BY THE BUDGET. A body is at most as large as the field
	 * lets it get and there are at most MaxBodiesOfEachKind of them, so this is
	 * tens of kilobytes for a wet level rather than an open-ended copy of the
	 * simulation. It is also rebuilt wholesale every reconcile, so it cannot drift
	 * out of step with the field the way a maintained mirror would.
	 */
	TSet<TPair<FIntPoint, int32>> Cells;

	/**
	 * Borrowed, never owned. The subsystem holds the fields and outlives every
	 * proxy; a region that survived its field would be answering questions about
	 * freed memory, which is why nothing here is allowed to keep one alive.
	 */
	FARPGFluidField* Field = nullptr;
};
