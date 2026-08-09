// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ARPGFluidBody.generated.h"

class UARPGElementalVolumeComponent;
class UARPGFluidDefinition;
class UARPGSolidDefinition;
class UBoxComponent;

/**
 * Shared behaviour of anything that IS an outline lying on the ground.
 *
 * A BODY IS ITS POLYGON. The ring (world XY, one simple outer ring) is the
 * single source of truth, and everything else is rebuilt from it whenever it
 * changes: the trigger bounds, the elemental volume's energy, whether it counts
 * as a reservoir. Storing anything derived alongside it is how the two get out
 * of step.
 */
UCLASS(Abstract)
class ARPGWORLD_API AARPGFluidBody : public AActor
{
	GENERATED_BODY()

public:
	AARPGFluidBody();

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

	/** Ground height the body formed at. */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Fluid")
	float GroundHeight = 0.f;

protected:
	/** Rebuilds the trigger box and the volume from the current ring. */
	virtual void RebuildFromRing();

	/** How far the surface sits above the ground. */
	virtual float GetSurfaceOffset() const { return 0.f; }

	UPROPERTY()
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
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Fluid")
	TObjectPtr<UARPGFluidDefinition> Definition;

	/** Configures the pool. Called by the subsystem; nothing else builds one. */
	void Setup(UARPGFluidDefinition* InDefinition, const TArray<FVector2D>& InRing,
		float InGroundHeight);

protected:
	virtual void RebuildFromRing() override;
	virtual float GetSurfaceOffset() const override;
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
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Fluid")
	TObjectPtr<UARPGSolidDefinition> Definition;

	/** Interior gaps, kept separate from the outline -- see the class comment. */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|Fluid")
	TArray<FVector2D> HoleRing;

	void Setup(UARPGSolidDefinition* InDefinition, const TArray<FVector2D>& InRing,
		float InGroundHeight);

	/** Inside the outline AND not inside a hole. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	bool IsStandableAt(FVector WorldPoint) const;

protected:
	virtual void RebuildFromRing() override;
	virtual float GetSurfaceOffset() const override;
};
