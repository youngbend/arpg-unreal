// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ARPGFluidPresentationSubsystem.generated.h"

class AARPGFluidRegion;
class UARPGFluidDefinition;
class UMaterialInstanceDynamic;
class UMaterialParameterCollection;
class UNiagaraComponent;
class UNiagaraSystem;
class UTexture2D;
class UTextureRenderTarget2D;

/**
 * One element's visible surface: a Niagara sheet and the buffers it reads.
 *
 * ONE PER ELEMENT AND NOT ONE FOR EVERYTHING, because water and lava are drawn
 * with different materials and there is nowhere in a single grid to say which
 * cell is which. Bounded hard -- see UARPGFluidPresentationSubsystem::MaxSheets
 * -- because each of these is a GPU simulation, not a decal.
 */
USTRUCT()
struct FARPGFluidSheet
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> Component;

	/** Surface normal and water height, written by the sim, read by the material. */
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> NormalRT;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> VelocityRT;

	/** The CPU field, as a picture. See FARPGFluidField::SampleWindow. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> FieldTexture;

	/**
	 * What this element is drawn with, as an instance we can write to.
	 *
	 * A DYNAMIC INSTANCE RATHER THAN THE ASSET, because the material has to be
	 * told which textures to read and those are made per sheet at runtime -- there
	 * is no asset-time answer to "which render target". Niagara can bind user
	 * parameters into a renderer's material, but that is a link somebody has to
	 * author and keep in step with three parameter names; this is the same job
	 * done where the names already live.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Material;

	/** Scratch for the upload, so a redraw does not allocate. */
	TArray<FFloat16Color> Pixels;

	/**
	 * The body this sheet is drawing, and the patch of world it covers.
	 *
	 * ANCHORED, NOT FOLLOWED. The box is worked out from the body's own outline
	 * when the sheet is made and then left alone -- the puddle is not going
	 * anywhere, so neither is its grid. Everything that made a viewer-centred
	 * window difficult came from the window moving: the surface is reconstructed
	 * by interpolating between vertices, so a grid sliding under a world-locked
	 * surface re-interpolates it from different points every frame and the water
	 * ripples wherever the player walks. Snapping only turned the slide into
	 * steps. A grid that does not move has none of it.
	 */
	FVector2D BoxCentre = FVector2D::ZeroVector;
	float BoxExtent = 0.f;

	/**
	 * Grid sizes chosen for THIS body, not for the biggest one imaginable.
	 *
	 * A sheet is now the size of its puddle, so a fixed resolution means a splash
	 * a few metres across is simulated and pictured at the same cost as a lake --
	 * measured at 1.6ms of CPU per body for a picture at two centimetres a texel,
	 * of a field whose cells are forty. Both are derived from a target cell size
	 * and capped, so the cost follows the water rather than the knob.
	 */
	int32 PictureTexels = 64;
	int32 SimCells = 128;

	/**
	 * Where the last upload was taken from, kept only so the dump can say.
	 *
	 * The material derives the same numbers from the parameter collection, and if
	 * those two ever disagree the water is drawn somewhere other than where it
	 * was sampled -- which looks like the mask being broken rather than like two
	 * answers to "where is the window", so it is worth being able to print both.
	 */
	FVector2D LastOrigin = FVector2D::ZeroVector;
	float LastNearZ = 0.f;

	/** Milliseconds spent turning the field into a picture, last time it happened. */
	double LastSampleMs = 0.0;
};

/**
 * The half of the fluid system that only draws.
 *
 * A GPU SHALLOW-WATER SHEET FOLLOWING THE VIEWER, and it decides nothing. The
 * simulation a puddle IS lives on the CPU, in FARPGFluidField, where the
 * reaction solver can ask it questions in the middle of a frame and an
 * automation fixture can step it with no renderer at all. The sheet's entire job
 * is to add the ripples, the foam and the surface normal that a forty-centimetre
 * grid cannot carry, and it is told what to add them to every step.
 *
 * SO IT IS A SEPARATE SUBSYSTEM, and that is the point of the split rather than
 * an accident of it. UARPGFluidSurfaceSubsystem is gated on authority because it
 * mutates world state; this one is gated on there being somebody to draw for, and
 * does not run on a dedicated server or under -nullrhi at all. Keeping them apart
 * means the presentation half can be absent entirely -- no renderer, no Niagara,
 * no textures -- without a single #if in the solver.
 *
 * WHICH IS ALSO THE TEST. Turn ARPG.Fluid.Presentation off and gameplay must be
 * byte-identical. If it is not, something has quietly started reading the picture
 * instead of the simulation, and that is the one failure this architecture exists
 * to make impossible.
 *
 * THE CONTRACT WITH THE ASSET IS WRITTEN DOWN TWICE, here and in
 * Docs/FLUID_NIAGARA_AUTHORING.md, because the other end of it lives inside a
 * Niagara system that no script can read and no compiler can check. Every name
 * below is a string that has to match something a person typed into a graph.
 */
UCLASS()
class ARPGWORLD_API UARPGFluidPresentationSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ USubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;
	//~ End USubsystem

	//~ UWorldSubsystem
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem

	//~ FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject

	/**
	 * Is the Niagara surface switched on?
	 *
	 * STATIC AND CHEAP, because what asks is every body of water on every rebuild:
	 * a region draws a plain slab of its own outline when the sheet is off, and
	 * hides it when the sheet is on, so that the two can never both be visible and
	 * turning the switch off can never leave the water invisible.
	 */
	static bool IsEnabled();

	/**
	 * What every sheet is drawing and what it was handed, as text.
	 *
	 * FOR ARPG.Fluid.DebugSheet. A sheet that draws the wrong thing looks exactly
	 * like a sheet that was handed the wrong thing, and from outside the process
	 * there is no way to tell which -- the picture is a transient texture and the
	 * material is a dynamic instance, so neither survives into anything you can
	 * open. This prints both ends: the material actually bound, and the range of
	 * the depths in the picture that material is masking itself against.
	 */
	FString DescribeSheets() const;

	/** Is anything actually being drawn right now? False is a legitimate answer. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	bool IsPresenting() const;

	/**
	 * How wide the simulated window is, in centimetres.
	 *
	 * WAS 2048, the engine's own shallow water default. Doubled because that is
	 * how far away the sheet stops: a body of water wider than this is cut off at
	 * the window's border, and the border is a square that moves with the viewer.
	 * A few overlapping casts make a puddle twenty metres across, so the default
	 * put that edge inside the frame most of the time.
	 *
	 * NOT FREE, BUT NEARLY. The field picture stays 128 texels, so this coarsens
	 * it from 16cm to 32cm per texel -- still finer than the 40cm cell it is a
	 * picture OF, so nothing is actually lost. The GPU sim's cells go from 4cm to
	 * 8cm, which costs detail in the ripples and no performance at all.
	 *
	 * AND IT DOES NOT REMOVE THE EDGE, only moves it. M_ARPG_Water fades the last
	 * tenth of the window out so that the water thins rather than ending; the two
	 * together are what stop it reading as a square.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Fluid",
		meta = (ClampMin = "256.0"))
	float WindowSize = 16384.f;

	/**
	 * Cells across that window. 512 over 2048cm is FOUR CENTIMETRES a cell.
	 *
	 * Ten times finer than the CPU field, and that ratio is the whole reason the
	 * sheet exists: at forty centimetres a wave is two cells and reads as a step.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Fluid",
		meta = (ClampMin = "64", ClampMax = "1024"))
	int32 Resolution = 512;

	/**
	 * Texels across the picture of the CPU field.
	 *
	 * SMALLER THAN THE SHEET ON PURPOSE. This carries what the simulation knows --
	 * depth, floor, what is standing on it -- at a resolution the CPU has to fill
	 * in every step, and 128 across two thousand centimetres is already finer than
	 * the forty-centimetre cells it is drawn from. Making it match the sheet would
	 * be paying four hundred times over to interpolate the same numbers.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Fluid",
		meta = (ClampMin = "32", ClampMax = "512"))
	int32 FieldTextureResolution = 512;

	/**
	 * How many elements may be drawn at once.
	 *
	 * TWO: the water you are standing in and the lava you are not. Each sheet is a
	 * GPU simulation of its own, and a third element visible at the same time is
	 * a situation that has never arisen in this game.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Fluid",
		meta = (ClampMin = "1", ClampMax = "8"))
	int32 MaxSheets = 4;

	/**
	 * How much dry ground to leave around a body, in centimetres.
	 *
	 * ROOM TO SPREAD. The grid is sized to the outline when the sheet is made, and
	 * a body that grows past its grid has to be given a new one -- which restarts
	 * that simulation and with it every ripple on it. The apron buys the time for
	 * a puddle to finish spreading without that happening, and it is also where
	 * the material's edge fade lives, so a body drawn right up to its own boundary
	 * would end in a hard line.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Fluid",
		meta = (ClampMin = "0.0"))
	float Apron = 400.f;

	/**
	 * Centimetres of world per texel of the field picture.
	 *
	 * FINER THAN THE FIELD IT PICTURES, and not much finer. The cells are forty
	 * centimetres, so sixteen already interpolates between them; going finer pays
	 * quadratically to invent detail that is not in the source.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Fluid",
		meta = (ClampMin = "2.0"))
	float TexelSize = 16.f;

	/** Centimetres of world per cell of the GPU simulation. Ripples live here. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Fluid",
		meta = (ClampMin = "1.0"))
	float SimCellSize = 4.f;

	/** Smallest grid a body gets, so a splash is not simulated at millimetres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Fluid",
		meta = (ClampMin = "128.0"))
	float MinSheetSize = 1024.f;

	/** The sheet's centre, or the origin when nothing is being drawn. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Fluid")
	FVector GetWindowCentre() const { return WindowCentre; }

protected:
	/** Builds or refreshes one element's sheet. False when content is missing. */
	bool UpdateSheet(AARPGFluidRegion* Region);

	/** Tears down every sheet without tearing the subsystem down. */
	void ReleaseSheets();

	void ReleaseSheet(FARPGFluidSheet& Sheet);

	/**
	 * Where the sheet should sit: the local viewer.
	 *
	 * THE Z IS KEPT, unlike everything else here, and it is the one number that
	 * decides whether the balcony or the room under it gets drawn. A flat picture
	 * can only show one floor -- see FARPGFluidField::FWindow::NearZ.
	 */
	bool FindViewer(FVector& OutCentre) const;

	UPROPERTY(Transient)
	TMap<TObjectPtr<AARPGFluidRegion>, FARPGFluidSheet> Sheets;

	/**
	 * How many grids have been built since the world started.
	 *
	 * EVERY ONE OF THESE IS A FLASH. Building a sheet throws away the body's
	 * simulation and its textures and starts over, so a number that climbs while
	 * nothing is happening means the hysteresis is not holding and the water is
	 * visibly blinking. Counted rather than reasoned about, because the first
	 * version of the growth test rebuilt every few centimetres and looked exactly
	 * like a rendering bug.
	 */
	int32 Rebuilds = 0;

	/** Of those, how many were an existing body outgrowing its box. */
	int32 Regrown = 0;

	/**
	 * Milliseconds spent building grids, and over how long.
	 *
	 * CHURN IS ONLY A PROBLEM IF IT COSTS SOMETHING. Building a sheet allocates
	 * two render targets, a texture and a component, and restarts a simulation --
	 * expensive per unit, but the question is what it adds up to per second, and
	 * that is worth knowing before rebuilding the region layer to avoid it.
	 */
	double RebuildMs = 0.0;
	double FirstRebuildAt = 0.0;

	/**
	 * Where the sheet is, for the material to line its samples up with.
	 *
	 * THREE NUMBERS IS ALL A COLLECTION CAN HOLD and all this needs -- the grids
	 * themselves are render targets bound straight onto the Niagara component, and
	 * the only thing a material cannot work out for itself is where in the world
	 * their corner currently is. Exactly the argument UARPGSpreadSubsystem's mask
	 * parameters already make.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> Parameters;

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraSystem> SheetSystem;

	FVector WindowCentre = FVector::ZeroVector;

	/**
	 * Where the window was LAST frame, which is what the material is told.
	 *
	 * THE SIM IS A FRAME BEHIND THE GAME. Niagara's GPU work for this frame runs
	 * after the game thread has already moved the component, so the grid the
	 * material samples was simulated around the previous centre. Publishing the
	 * current one puts the water a frame's worth of movement away from where it is
	 * drawn, which reads as the whole surface sliding under the player when they
	 * run. The engine's own shallow water subsystem does exactly this.
	 */
	FVector PublishedCentre = FVector::ZeroVector;

	/** So "no system is configured" is said once rather than sixty times a second. */
	bool bWarnedNoSystem = false;
};
