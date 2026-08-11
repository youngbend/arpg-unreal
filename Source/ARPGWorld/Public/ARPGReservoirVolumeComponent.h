// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGFreezableSurface.h"
#include "ARPGReservoirVolumeComponent.generated.h"

class UWaterBodyComponent;

/**
 * A standing body of water that was PLACED rather than spawned -- a river, a
 * lake, a moat -- made freezable.
 *
 * WHY A RESERVOIR IS NOT A FLUID POOL, and should not become one. The fluid
 * surface subsystem exists for bodies that appear and change shape at runtime:
 * every operation on one is a polygon rewrite, and it charges the whole system's
 * cost to how often that happens. A river does none of that. It is authored, it
 * is static, and it wants spline fitting, terrain carving and a real water
 * shader -- all of which Unreal's Water plugin already does far better than a
 * polygon-per-tick simulation ever would.
 *
 * So the two stay separate, and what makes a river part of the elemental world is
 * only this: a volume component saying what it is made of. That was always the
 * design -- see UARPGElementalVolumeComponent, whose PROJECTILE/RESERVOIR split
 * is exactly this distinction -- and the only thing missing was that nothing
 * authored could be frozen.
 *
 * FREEZING IS WRITTEN AGAINST CONTAINMENT, NOT AGAINST A SHAPE. The footprint is
 * found by marching outward from the contact point until the water stops, asking
 * only ContainsPoint. That means this class needs no knowledge of splines, boxes
 * or meshes: a subclass that can answer "is this point in me" more accurately
 * gets a more accurate frozen patch for free, and the base box works well enough
 * to test against.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGWORLD_API UARPGReservoirVolumeComponent : public UARPGElementalVolumeComponent,
	public IARPGFreezableSurface
{
	GENERATED_BODY()

public:
	UARPGReservoirVolumeComponent();

	//~ IARPGFreezableSurface
	virtual TArray<FVector2D> GetFreezableFootprint(const FVector2D& Centre, double Radius) const override;
	virtual float GetFreezableSurfaceHeight(const FVector2D& At) const override;
	virtual bool ConsumeFreezableArea(double Area) override;
	//~ End IARPGFreezableSurface

	/**
	 * How many directions the footprint march samples.
	 *
	 * The only resolution knob here, and it governs how closely a frozen patch
	 * hugs an irregular bank. Freezing happens on a spell impact rather than on a
	 * tick, so this can afford to be generous.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Reservoir",
		meta = (ClampMin = "6", ClampMax = "64"))
	int32 FootprintSegments = 16;

protected:
	/** Is there water at this XY, tested just under the surface? */
	bool IsWaterAt(const FVector2D& Point) const;

	/** How far the water reaches from Centre along Direction, up to MaxDistance. */
	double MarchToEdge(const FVector2D& Centre, const FVector2D& Direction, double MaxDistance) const;
};

/**
 * A reservoir backed by a Water plugin body, so a river is a spline rather than
 * a box.
 *
 * THIS IS THE OPPOSITE CALL FROM THE ONE MADE FOR PUDDLES, for a consistent
 * reason. The line is not water against not-water; it is AUTHORED AND STATIC
 * against SPAWNED AND RESHAPED. Water bodies are spline-authored level geometry
 * served by a water zone, and asking one to be respawned and rebuilt four times a
 * second is a fight. Asking it to be a river is what it is for.
 *
 * WHAT THIS ACTUALLY BUYS is the two answers the base class has to guess at. A
 * box's idea of containment is its axis-aligned bounds, which for a river that
 * bends covers the whole valley -- everyone in it soaked, every fireball over it
 * detonating. And a box's idea of a waterline is one authored number, which
 * cannot describe a river running downhill. The water body knows both exactly,
 * because the same data drives its own buoyancy.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGWORLD_API UARPGWaterBodyVolumeComponent : public UARPGReservoirVolumeComponent
{
	GENERATED_BODY()

public:
	UARPGWaterBodyVolumeComponent();

	virtual void BeginPlay() override;

	virtual bool ContainsPoint(FVector WorldPoint, float BelowReach = 0.f) const override;
	virtual float GetSurfaceHeightAt(FVector WorldPoint) const override;

	/**
	 * The body to ask. Defaults to the one on this component's owner, which is
	 * the normal case: drop this onto an AWaterBodyRiver and it wires itself.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Reservoir")
	TObjectPtr<UWaterBodyComponent> WaterBody;

protected:
	/** Finds the owner's water body when none was assigned. Warns if there is none. */
	void ResolveWaterBody();
};
