// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGFluidPresentationSubsystem.h"

#include "ARPGFluidDefinition.h"
#include "ARPGFluidField.h"
#include "ARPGFluidGeometry.h"
#include "ARPGFluidRegion.h"
#include "ARPGFluidSurfaceSubsystem.h"
#include "ARPGWorld.h"
#include "ARPGWorldSettings.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "UnrealClient.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

namespace
{
	/**
	 * OFF BY DEFAULT, and it stays off until the sheet is something worth looking
	 * at. The whole point of splitting presentation out is that the game is
	 * complete without it; a switch that started on would make that claim
	 * untestable on the first day it stopped being true.
	 */
	static TAutoConsoleVariable<int32> CVarFluidPresentation(
		TEXT("ARPG.Fluid.Presentation"),
		0,
		TEXT("Draw fluids with the Niagara shallow-water sheet.\n")
		TEXT("  0: off -- the CPU field still simulates, bodies draw a plain slab (default)\n")
		TEXT("  1: on\n")
		TEXT("Gameplay must be identical either way. If it is not, something is\n")
		TEXT("reading the picture instead of the simulation."),
		ECVF_Default);

	/**
	 * The user parameters the sheet exposes.
	 *
	 * EVERY ONE OF THESE IS A STRING SOMEBODY TYPED INTO A GRAPH. Nothing checks
	 * them: a misspelling here is a silent no-op at runtime, not a compile error,
	 * which is why the same list is written out in Docs/FLUID_NIAGARA_AUTHORING.md
	 * and why that document is part of the work rather than a nicety.
	 */
	namespace SheetParams
	{
		static const FName WorldGridSize(TEXT("WorldGridSize"));
		static const FName ResolutionMaxAxis(TEXT("ResolutionMaxAxis"));
		static const FName NormalRT(TEXT("NormalRT"));
		static const FName VelocityRT(TEXT("VelocityRT"));

		/** The CPU field as a picture -- see FARPGFluidField::SampleWindow. */
		static const FName FieldTexture(TEXT("FieldTexture"));
		static const FName FieldOrigin(TEXT("FieldOrigin"));
		static const FName FieldExtent(TEXT("FieldExtent"));
		static const FName FieldTexel(TEXT("FieldTexel"));

		/** How hard the sheet is pulled back towards what the field says. */
		static const FName ConstrainRate(TEXT("ConstrainRate"));

		/** What this element is drawn with. Water and lava differ here and nowhere else. */
		static const FName SurfaceMaterial(TEXT("SurfaceMaterial"));
	}

	/**
	 * And the ones the material reads. DELIBERATELY THE ENGINE'S OWN NAMES -- see
	 * UShallowWaterSettings, which defaults GridCenterMPCName to SimLocation,
	 * WorldGridSizeMPCName to FluidSimSize and ResolutionMaxAxisMPCName to
	 * FluidSimResolution. Matching them means the material functions Epic ships
	 * under NiagaraFluids/Content/Materials/ShallowWater work unmodified, which is
	 * several days of not re-deriving somebody else's water shader.
	 */
	namespace MaterialParams
	{
		static const FName SimLocation(TEXT("SimLocation"));
		static const FName FluidSimSize(TEXT("FluidSimSize"));
		static const FName FluidSimResolution(TEXT("FluidSimResolution"));
		static const FName FieldOrigin(TEXT("FieldOrigin"));
		static const FName FieldExtent(TEXT("FieldExtent"));
	}

	UTextureRenderTarget2D* MakeTarget(UObject* Outer, int32 Size,
		ETextureRenderTargetFormat Format)
	{
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(Outer);

		if (!Target)
		{
			return nullptr;
		}

		Target->RenderTargetFormat = Format;
		Target->ClearColor = FLinearColor::Black;
		Target->bAutoGenerateMips = false;

		// CLAMPED, because the window ends. Wrapping would put the water from the
		// far side of the sheet under the player's feet at the near edge, which is
		// the same argument the spread mask already makes for its own window.
		Target->AddressX = TA_Clamp;
		Target->AddressY = TA_Clamp;

		Target->InitAutoFormat(Size, Size);
		Target->UpdateResourceImmediate(true);

		return Target;
	}

	UTexture2D* MakeFieldTexture(int32 Size)
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(Size, Size, PF_FloatRGBA);

		if (!Texture)
		{
			return nullptr;
		}

		// BILINEAR IS THE WHOLE POINT, not a nicety -- the same argument the spread
		// mask makes. Read per texel, a puddle's edge steps along texel boundaries
		// and the same puddle looks different depending on where the viewer is
		// standing relative to one, which reads as the simulation being unreliable
		// rather than as aliasing.
		Texture->Filter = TF_Bilinear;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->SRGB = false;
		Texture->NeverStream = true;
		Texture->AddToRoot();
		Texture->UpdateResource();

		return Texture;
	}
}

bool UARPGFluidPresentationSubsystem::IsEnabled()
{
	return CVarFluidPresentation.GetValueOnGameThread() != 0;
}

bool UARPGFluidPresentationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// Nobody to draw for. A listen server still makes one -- somebody is watching
	// on that machine -- which is the same line UARPGStatusVfxSubsystem draws.
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->GetNetMode() != NM_DedicatedServer;
}

void UARPGFluidPresentationSubsystem::Deinitialize()
{
	ReleaseSheets();
	Super::Deinitialize();
}

bool UARPGFluidPresentationSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UARPGFluidPresentationSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UARPGFluidPresentationSubsystem, STATGROUP_Tickables);
}

bool UARPGFluidPresentationSubsystem::IsPresenting() const
{
	for (const TPair<TObjectPtr<AARPGFluidRegion>, FARPGFluidSheet>& Pair : Sheets)
	{
		if (Pair.Value.Component && Pair.Value.Component->IsActive())
		{
			return true;
		}
	}

	return false;
}

void UARPGFluidPresentationSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!IsEnabled())
	{
		// TORN DOWN RATHER THAN DEACTIVATED. Turning the switch off is a claim that
		// this costs nothing, and a deactivated component still holds two render
		// targets and a GPU sim's worth of buffers.
		ReleaseSheets();
		return;
	}

	UWorld* World = GetWorld();
	UARPGFluidSurfaceSubsystem* Fluids =
		World ? World->GetSubsystem<UARPGFluidSurfaceSubsystem>() : nullptr;

	if (!Fluids)
	{
		return;
	}

	FVector Centre;

	if (!FindViewer(Centre))
	{
		return;
	}

	// LAST FRAME'S CENTRE GOES OUT, THIS FRAME'S GOES IN, and the order is the
	// whole of it -- see the header. The grid the material samples this frame was
	// simulated around where the window was when the GPU last ran, which is the
	// value WindowCentre still holds at this point.
	PublishedCentre = WindowCentre;
	WindowCentre = Centre;

	if (Parameters)
	{
		const float Extent = WindowSize;
		const FVector2D Origin(PublishedCentre.X - Extent * 0.5f,
			PublishedCentre.Y - Extent * 0.5f);

		UKismetMaterialLibrary::SetVectorParameterValue(World, Parameters,
			MaterialParams::SimLocation,
			FLinearColor(PublishedCentre.X, PublishedCentre.Y, PublishedCentre.Z, 0.f));
		UKismetMaterialLibrary::SetScalarParameterValue(World, Parameters,
			MaterialParams::FluidSimSize, WindowSize);
		UKismetMaterialLibrary::SetScalarParameterValue(World, Parameters,
			MaterialParams::FluidSimResolution, static_cast<float>(Resolution));
		UKismetMaterialLibrary::SetVectorParameterValue(World, Parameters,
			MaterialParams::FieldOrigin, FLinearColor(Origin.X, Origin.Y, 0.f, 0.f));
		UKismetMaterialLibrary::SetScalarParameterValue(World, Parameters,
			MaterialParams::FieldExtent, Extent);
	}

	// WHICH BODIES ARE WORTH DRAWING.
	//
	// ONE SHEET PER BODY, sized and anchored to that body. The alternative was one
	// window centred on the viewer, and everything awkward about the sheet came
	// from it: the grid slid as you walked so the surface flapped, the window's
	// square edge had to be faded away, and a puddle you were not standing near
	// was simply not drawn -- which no amount of tuning was going to fix.
	//
	// BIGGEST FIRST. Each of these is a GPU simulation, so there is a budget; when
	// it is full the bodies that miss out keep drawing the flat slab they draw
	// with presentation off. Choosing by volume rather than by distance means the
	// lake behind you keeps its sheet while a splash at your feet does not, which
	// is the right way round: the big one is what you would notice losing.
	TArray<AARPGFluidRegion*> Wanted;

	for (AARPGFluidRegion* Region : Fluids->GetRegions())
	{
		if (IsValid(Region) && Region->GetVolume() > 0.0 && Region->GetRing().Num() >= 3)
		{
			Wanted.Add(Region);
		}
	}

	Wanted.Sort([](const AARPGFluidRegion& A, const AARPGFluidRegion& B)
		{
			return A.GetVolume() > B.GetVolume();
		});

	if (Wanted.Num() > MaxSheets)
	{
		Wanted.SetNum(MaxSheets);
	}

	for (auto It = Sheets.CreateIterator(); It; ++It)
	{
		if (!Wanted.Contains(It.Key()))
		{
			if (AARPGFluidRegion* Region = It.Key())
			{
				Region->SetDrawnBySheet(false);
			}

			ReleaseSheet(It.Value());
			It.RemoveCurrent();
		}
	}

	for (AARPGFluidRegion* Region : Wanted)
	{
		Region->SetDrawnBySheet(UpdateSheet(Region));
	}
}

bool UARPGFluidPresentationSubsystem::FindViewer(FVector& OutCentre) const
{
	const UWorld* World = GetWorld();

	if (!World)
	{
		return false;
	}

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* Controller = It->Get();

		if (!Controller || !Controller->IsLocalController())
		{
			continue;
		}

		const AActor* View = Controller->GetPawn()
			? Cast<AActor>(Controller->GetPawn())
			: Controller->GetViewTarget();

		if (!View)
		{
			continue;
		}

		OutCentre = View->GetActorLocation();
		return true;
	}

	return false;
}

bool UARPGFluidPresentationSubsystem::UpdateSheet(AARPGFluidRegion* Region)
{
	UWorld* World = GetWorld();

	if (!World || !IsValid(Region))
	{
		return false;
	}

	UARPGFluidDefinition* Definition = Region->Definition;

	UARPGFluidSurfaceSubsystem* Fluids = World->GetSubsystem<UARPGFluidSurfaceSubsystem>();
	const FARPGFluidField* Field = (Fluids && Definition)
		? Fluids->FindField(Definition) : nullptr;

	if (!Field)
	{
		return false;
	}

	// --- The patch of world this sheet covers ---------------------------------
	//
	// THE BODY'S OWN OUTLINE PLUS ROOM TO SPREAD, worked out once and then left
	// alone. Re-deriving it every frame would put the grid back to chasing
	// something, which is the whole problem this replaced.
	//
	// SQUARE, because the grid is. A long thin puddle gets a grid as wide as it is
	// long and wastes the difference; the alternative is a non-square grid, and
	// the resolution knob and every module that reads it assume one number.
	const FBox2D Outline = ARPGFluidGeometry::PolygonBounds(Region->GetRing());

	const float Span = static_cast<float>(
		FMath::Max(Outline.GetSize().X, Outline.GetSize().Y));

	const FVector2D Middle = Outline.GetCenter();

	FARPGFluidSheet& Sheet = Sheets.FindOrAdd(Region);

	// GROWN, NOT RESIZED, and only when the body has actually eaten its apron.
	//
	// THE FIRST VERSION REBUILT CONSTANTLY. It compared the box it WANTED against
	// the box it had, and since the wanted size grows with the puddle, a spreading
	// body asked for a new grid every few centimetres -- and every rebuild throws
	// away that body's simulation and its textures and starts again, which is a
	// visible flash. The apron only buys hysteresis if the test is against the
	// apron rather than against the exact fit.
	const bool bOutgrown = Sheet.Component
		&& (Span > Sheet.BoxExtent - Apron
			|| FVector2D::Distance(Middle, Sheet.BoxCentre) > Apron);

	if (bOutgrown)
	{
		++Regrown;
		ReleaseSheet(Sheet);
	}

	if (!Sheet.Component)
	{
		// A FULL APRON ON EACH SIDE of whatever the body is now, so the rebuild
		// that just happened buys room for the next stretch of spreading rather
		// than tripping again immediately.
		Sheet.BoxCentre = Middle;
		Sheet.BoxExtent = FMath::Clamp(Span + 2.f * Apron, MinSheetSize, WindowSize);

		const float Needed = Sheet.BoxExtent;

		// SIZED TO THIS BODY. Rounded up to a power of two so the two grids stay
		// in step with each other and a texel is always a whole number of cells.
		Sheet.PictureTexels = FMath::Clamp(
			FMath::RoundUpToPowerOfTwo(FMath::CeilToInt(Needed / TexelSize)),
			32, FieldTextureResolution);

		Sheet.SimCells = FMath::Clamp(
			FMath::RoundUpToPowerOfTwo(FMath::CeilToInt(Needed / SimCellSize)),
			64, Resolution);
	}

	const UARPGWorldSettings& Settings = UARPGWorldSettings::Get();

	if (!SheetSystem)
	{
		SheetSystem = Settings.FluidSheetSystem.LoadSynchronous();
	}

	if (!SheetSystem)
	{
		// A LEGITIMATE CONFIGURATION until the sheet asset is authored by hand, so
		// this is a warning said once rather than an error said every frame. Until
		// then bodies draw the plain slab they draw with presentation off, which is
		// why turning this on cannot make the water disappear.
		if (!bWarnedNoSystem)
		{
			bWarnedNoSystem = true;
			UE_LOG(LogARPGWorld, Warning,
				TEXT("ARPG.Fluid.Presentation is on but no FluidSheetSystem is set in ")
				TEXT("ARPG World settings. See Docs/FLUID_NIAGARA_AUTHORING.md."));
		}

		return false;
	}

	if (!Parameters)
	{
		Parameters = Settings.FluidParameters.LoadSynchronous();
	}

	if (!Sheet.Component)
	{
		Sheet.NormalRT = MakeTarget(this, Sheet.SimCells, RTF_RGBA16f);
		Sheet.VelocityRT = MakeTarget(this, Sheet.SimCells, RTF_RG16f);
		Sheet.FieldTexture = MakeFieldTexture(Sheet.PictureTexels);

		if (!Sheet.NormalRT || !Sheet.VelocityRT || !Sheet.FieldTexture)
		{
			ReleaseSheet(Sheet);
			return false;
		}

		// NOT ATTACHED TO AN ACTOR. There is no actor this belongs to -- the sheet
		// is a property of the world, not of anything standing in it -- and
		// inventing one would put a spawned actor in the level for everything else
		// to trip over. A component registered straight with the world is what the
		// engine's own shallow water subsystem does, for the same reason.
		++Rebuilds;

		const double BuildStart = FPlatformTime::Seconds();

		if (FirstRebuildAt <= 0.0)
		{
			FirstRebuildAt = BuildStart;
		}

		Sheet.Component = NewObject<UNiagaraComponent>(this);
		Sheet.Component->SetAsset(SheetSystem);
		Sheet.Component->SetAutoActivate(false);
		Sheet.Component->SetAutoDestroy(false);
		Sheet.Component->SetAbsolute(true, true, true);
		Sheet.Component->RegisterComponentWithWorld(World);

		Sheet.Component->SetVariableVec2(SheetParams::WorldGridSize,
			FVector2D(Sheet.BoxExtent, Sheet.BoxExtent));
		Sheet.Component->SetVariableInt(SheetParams::ResolutionMaxAxis, Sheet.SimCells);
		Sheet.Component->SetVariableTextureRenderTarget(SheetParams::NormalRT, Sheet.NormalRT);
		Sheet.Component->SetVariableTextureRenderTarget(SheetParams::VelocityRT, Sheet.VelocityRT);
		Sheet.Component->SetVariableTexture(SheetParams::FieldTexture, Sheet.FieldTexture);
		Sheet.Component->SetVariableFloat(SheetParams::ConstrainRate, 6.f);

		// WHAT THIS ELEMENT LOOKS LIKE, and the only thing that differs between the
		// water sheet and the lava one. Everything else about them is identical,
		// which is the point of driving both from the same system.
		//
		// AND WHERE IT READS THE SIMULATION FROM. The surface material masks itself
		// against the field texture -- that is what stops the sheet being a
		// two-thousand-centimetre square of water-coloured ground -- so it needs
		// the same three textures this sheet just made. Set on an instance here
		// rather than bound through the Niagara renderer, because these objects do
		// not exist until now and there is no asset-time answer to give.
		if (UMaterialInterface* Asset = Definition->SurfaceMaterial.LoadSynchronous())
		{
			Sheet.Material = UMaterialInstanceDynamic::Create(Asset, this);

			if (Sheet.Material)
			{
				Sheet.Material->SetTextureParameterValue(
					SheetParams::FieldTexture, Sheet.FieldTexture);
				Sheet.Material->SetTextureParameterValue(
					SheetParams::NormalRT, Sheet.NormalRT);
				Sheet.Material->SetTextureParameterValue(
					SheetParams::VelocityRT, Sheet.VelocityRT);

				Sheet.Component->SetVariableMaterial(
					SheetParams::SurfaceMaterial, Sheet.Material);
			}
		}

		Sheet.Component->Activate(true);

		RebuildMs += (FPlatformTime::Seconds() - BuildStart) * 1000.0;
	}

	// --- The picture of the field ------------------------------------------

	FARPGFluidField::FWindow Window;
	Window.Extent = Sheet.BoxExtent;
	Window.Resolution = Sheet.PictureTexels;

	// NO SNAPPING, BECAUSE NOTHING MOVES. The box was fixed when the sheet was
	// made and the puddle is not going anywhere, so the picture is taken from the
	// same place every time by construction. A viewer-centred window needed the
	// origin snapped to a cell or the field resampled at a different sub-cell
	// offset each frame and the whole surface crawled -- and snapping the picture
	// alone was not enough either, because the geometry was still sliding under a
	// world-locked surface. Both of those were the window moving.
	Window.Origin = Sheet.BoxCentre - FVector2D(Window.Extent * 0.5f, Window.Extent * 0.5f);

	// THE BODY'S OWN FLOOR -- see FWindow::NearZ. A flat picture shows one floor,
	// and for a sheet that exists to draw THIS puddle the honest one is the floor
	// the puddle is lying on, not whichever one the camera happens to be above.
	Window.NearZ = Field->SampleBed(Sheet.BoxCentre);

	// TIMED, BECAUSE THE WINDOW IS A TRADE AND NOBODY SHOULD GUESS AT IT. This
	// walks Resolution^2 texels through a sparse chunk lookup every field step, so
	// it is the one cost that grows when the sheet is made to cover more ground --
	// the GPU sim does not care how wide its cells are, and the mesh is a fixed
	// 32k triangles either way. Doubling the window at the same texel density is
	// four times this number and nothing else.
	const double SampleStart = FPlatformTime::Seconds();

	Field->SampleWindow(Window, Sheet.Pixels, &Region->GetCells());

	Sheet.LastSampleMs = (FPlatformTime::Seconds() - SampleStart) * 1000.0;

	// --- Handing the picture to the GPU ---------------------------------------
	//
	// NOT UpdateResource(). That was the first thing that worked and it is a
	// RECREATE, not an update: it tears the RHI texture down and builds a new one
	// from the platform data, every field step, four times a second. In between,
	// the material samples a texture that is not there -- the mask reads nothing
	// and patches of the puddle blink out, and the FLOOR reads nothing, which puts
	// vertices at a height taken from noise and throws sheets of water into the
	// air. Both of those look like simulation bugs and neither is.
	//
	// A COPY PER UPLOAD, because the region update is consumed on the render
	// thread and Sheet.Pixels is overwritten by the next step long before that.
	// Freed by the cleanup callback, which is the only thing that knows when the
	// GPU is done with it.
	const int32 Bytes = Sheet.Pixels.Num() * sizeof(FFloat16Color);

	if (Bytes > 0)
	{
		uint8* Copy = static_cast<uint8*>(FMemory::Malloc(Bytes));
		FMemory::Memcpy(Copy, Sheet.Pixels.GetData(), Bytes);

		FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(
			0, 0, 0, 0, Window.Resolution, Window.Resolution);

		Sheet.FieldTexture->UpdateTextureRegions(
			0, 1, Region,
			Window.Resolution * sizeof(FFloat16Color),
			sizeof(FFloat16Color),
			Copy,
			[](uint8* Data, const FUpdateTextureRegion2D* Done)
			{
				FMemory::Free(Data);
				delete Done;
			});
	}

	Sheet.LastOrigin = Window.Origin;
	Sheet.LastNearZ = Window.NearZ;

	// THE PICTURE AND WHERE IT IS, SET TOGETHER. The material masks itself against
	// the field by turning a world position into a texel, and it can only do that
	// if it is told the same window the texture was just sampled from. Publishing
	// that through the parameter collection meant a GUID that had to resolve and a
	// frame of lag against an unsnapped centre; setting it on the instance that
	// already holds the texture means the two cannot disagree.
	if (Sheet.Material)
	{
		Sheet.Material->SetVectorParameterValue(SheetParams::FieldOrigin,
			FLinearColor(Window.Origin.X, Window.Origin.Y, 0.f, 0.f));
		Sheet.Material->SetScalarParameterValue(SheetParams::FieldExtent, Window.Extent);

		// ONE TEXEL, IN UV. The material compares this pixel's floor against the
		// floor a texel away to find the drops it must not draw across, and only
		// this side knows how many texels this body's picture has.
		Sheet.Material->SetScalarParameterValue(SheetParams::FieldTexel,
			1.f / FMath::Max(1, Sheet.PictureTexels));
	}

	Sheet.Component->SetVariableVec2(SheetParams::FieldOrigin, Window.Origin);
	Sheet.Component->SetVariableFloat(SheetParams::FieldExtent, Window.Extent);
	// A BASE PLANE, NOT THE WATER LINE. Where the surface is actually drawn is
	// decided per texel in the material, which raises each vertex onto
	// FieldTexture.G + FieldTexture.R -- the field's own floor plus its own depth.
	// Nothing here can put the water at the wrong height any more, and a puddle
	// down a slope slopes.
	//
	// So this only has to be CLOSE, and close is worth having: it keeps the
	// displacement small near the player, which keeps both the vertex precision
	// and the renderer's bounds honest. It went through Z=0 (water at the bottom
	// of the level) and then through the viewer's own Z (water bobbing when they
	// jumped) before the material took the job over. The floor under their feet is
	// the closest guess available from one number.
	const float Floor = Field->SampleBed(Sheet.BoxCentre);

	// WHERE THE PUDDLE IS, and it will still be there next frame.
	Sheet.Component->SetWorldLocation(
		FVector(Sheet.BoxCentre.X, Sheet.BoxCentre.Y, Floor));

	return true;
}

FString UARPGFluidPresentationSubsystem::DescribeSheets() const
{
	TStringBuilder<4096> Out;

	Out.Appendf(TEXT("presentation %s: %d sheet(s), window %.0fcm, sim %d, picture %d\n"),
		IsEnabled() ? TEXT("ON") : TEXT("OFF"), Sheets.Num(), WindowSize,
		Resolution, FieldTextureResolution);

	// SPLIT, BECAUSE THE TWO HAVE DIFFERENT CURES. A body outgrowing its box is
	// the hysteresis being too tight; a body appearing and vanishing is the region
	// layer churning underneath, and no amount of apron fixes that.
	const double Elapsed = FirstRebuildAt > 0.0
		? FPlatformTime::Seconds() - FirstRebuildAt : 0.0;

	Out.Appendf(
		TEXT("  %d grids built (%d regrown, %d new bodies), %.1fms total, %.2fms/s\n"),
		Rebuilds, Regrown, Rebuilds - Regrown, RebuildMs,
		Elapsed > 0.0 ? RebuildMs / Elapsed : 0.0);

	Out.Appendf(TEXT("  system %s, collection %s\n"),
		SheetSystem ? *SheetSystem->GetName() : TEXT("NONE"),
		Parameters ? *Parameters->GetName() : TEXT("NONE"));

	// WHAT THE MATERIAL WILL HAVE USED, not what the sheet was given. The
	// collection is published a frame behind on purpose -- see Tick -- so these
	// two disagreeing by one step is correct and by anything else is a bug.
	Out.Appendf(TEXT("  centre %s, published %s\n"),
		*WindowCentre.ToCompactString(), *PublishedCentre.ToCompactString());

	// EVERY ELEMENT, NOT EVERY SHEET. A sheet exists only where this subsystem
	// decided there was something to draw, so listing sheets alone can only ever
	// report what it already believes -- and when the belief is the bug, that is
	// the one thing you cannot see. Ask the field directly.
	if (const UARPGFluidSurfaceSubsystem* Fluids =
			GetWorld() ? GetWorld()->GetSubsystem<UARPGFluidSurfaceSubsystem>() : nullptr)
	{
		for (const TObjectPtr<UARPGFluidDefinition>& Definition : Fluids->Definitions)
		{
			const FARPGFluidField* Field = Fluids->FindField(Definition);

			if (!Definition || !Field)
			{
				Out.Appendf(TEXT("  %s: NO FIELD\n"),
					Definition ? *Definition->GetName() : TEXT("<null definition>"));
				continue;
			}

			const FVector2D At(WindowCentre.X, WindowCentre.Y);

			// HOW MANY OF THIS ELEMENT'S BODIES GOT A SHEET. One sheet per body
			// now, so an element is not drawn or not drawn -- some of it is.
			int32 Drawn = 0;

			for (const TPair<TObjectPtr<AARPGFluidRegion>, FARPGFluidSheet>& Pair : Sheets)
			{
				if (Pair.Key && Pair.Key->Definition == Definition)
				{
					++Drawn;
				}
			}

			Out.Appendf(
				TEXT("  %s: %.0f total in %d wet cells, %.0f within %.0fcm, sheet %s\n"),
				*Definition->GetName(), Field->GetTotalVolume(), Field->GetWetCellCount(),
				Field->GetVolumeWithin(At, WindowSize), WindowSize,
				*FString::FromInt(Drawn));
		}
	}

	for (const TPair<TObjectPtr<AARPGFluidRegion>, FARPGFluidSheet>& Pair : Sheets)
	{
		const FARPGFluidSheet& Sheet = Pair.Value;

		float Deepest = 0.f;
		double Total = 0.0;
		int32 Wet = 0;

		for (const FFloat16Color& Pixel : Sheet.Pixels)
		{
			const float Depth = Pixel.R.GetFloat();

			Deepest = FMath::Max(Deepest, Depth);
			Total += Depth;
			Wet += Depth > 0.f ? 1 : 0;
		}

		Out.Appendf(TEXT("  %s\n"),
			Pair.Key ? *Pair.Key->GetName() : TEXT("<null definition>"));

		Out.Appendf(TEXT("    component %s at %s\n"),
			Sheet.Component
				? (Sheet.Component->IsActive() ? TEXT("active") : TEXT("INACTIVE"))
				: TEXT("MISSING"),
			Sheet.Component
				? *Sheet.Component->GetComponentLocation().ToCompactString()
				: TEXT("-"));

		// THE PARENT IS THE INTERESTING HALF. Every sheet's instance is unique and
		// its name says nothing; what tells you whether the element got its own
		// look is which asset it was made from.
		// HOW BIG THE THING ON SCREEN IS. The mesh is 100cm and has to be scaled up
		// by the renderer to cover the window; nothing has ever checked that it is.
		// A sheet drawn at 1:1 is a one-metre tile under the player's feet, which
		// from anywhere except directly overhead is indistinguishable from nothing
		// being drawn at all.
		if (Sheet.Component)
		{
			const FBoxSphereBounds Bounds = Sheet.Component->Bounds;

			Out.Appendf(
				TEXT("    bounds %.0f x %.0f x %.0f at %s, visible %s, registered %s\n"),
				Bounds.BoxExtent.X * 2.f, Bounds.BoxExtent.Y * 2.f, Bounds.BoxExtent.Z * 2.f,
				*Bounds.Origin.ToCompactString(),
				Sheet.Component->IsVisible() ? TEXT("yes") : TEXT("NO"),
				Sheet.Component->IsRegistered() ? TEXT("yes") : TEXT("NO"));
		}

		// AND WHETHER THE MATERIAL IS LOOKING AT THE PICTURE. Setting a texture
		// parameter is silent whether or not the name matches anything.
		if (Sheet.Material)
		{
			UTexture* Bound = nullptr;
			Sheet.Material->GetTextureParameterValue(
				FMaterialParameterInfo(SheetParams::FieldTexture), Bound);

			Out.Appendf(TEXT("    FieldTexture parameter -> %s (want %s)\n"),
				Bound ? *Bound->GetName() : TEXT("NOTHING"),
				Sheet.FieldTexture ? *Sheet.FieldTexture->GetName() : TEXT("-"));
		}

		Out.Appendf(TEXT("    material %s\n"),
			Sheet.Material && Sheet.Material->Parent
				? *Sheet.Material->Parent->GetName()
				: TEXT("NONE -- the renderer will be using its own"));

		// WHAT IS ON THE SCREEN, not what we asked for. SetVariableMaterial hands a
		// material to a user parameter; whether the RENDERER uses it depends on a
		// binding stored in the emitter asset, and an inherited emitter re-merges
		// that binding from its parent every time it loads. So the value we set
		// and the value being drawn are two different questions, and only this one
		// can be answered by looking at the pixels -- which is exactly the way of
		// answering it that keeps being wrong.
		TArray<UMaterialInterface*> Used;
		Sheet.Component->GetUsedMaterials(Used);

		if (Used.IsEmpty())
		{
			Out.Append(TEXT("    RENDERING WITH: nothing\n"));
		}

		for (UMaterialInterface* Material : Used)
		{
			if (!Material)
			{
				Out.Append(TEXT("    RENDERING WITH: null\n"));
				continue;
			}

			const UMaterial* Base = Material->GetMaterial();

			Out.Appendf(TEXT("    RENDERING WITH: %s (base %s, niagara mesh usage %s)\n"),
				*Material->GetName(),
				Base ? *Base->GetName() : TEXT("?"),
				Base && Base->GetUsageByFlag(MATUSAGE_NiagaraMeshParticles)
					? TEXT("yes") : TEXT("NO"));
		}

		Out.Appendf(TEXT("    sampling the field into the picture cost %.2fms\n"),
			Sheet.LastSampleMs);

		// THE TERRAIN, AS THE SHEET SEES IT. The sim puts every vertex at bottom
		// contour plus depth, and the bottom contour is this channel -- so if it
		// does not vary, the surface CANNOT follow the ground however good the
		// simulation is. Reported over the whole window and over the wet part
		// separately, because the field only learns a cell's floor once it has had
		// water on it: a flat reading everywhere means the picture has no terrain
		// in it, and a flat reading over the wet part means the water has not
		// actually gone anywhere with a different floor.
		float BedLow = TNumericLimits<float>::Max();
		float BedHigh = -TNumericLimits<float>::Max();
		float WetBedLow = TNumericLimits<float>::Max();
		float WetBedHigh = -TNumericLimits<float>::Max();

		for (const FFloat16Color& Pixel : Sheet.Pixels)
		{
			const float Bottom = Pixel.B.GetFloat();

			BedLow = FMath::Min(BedLow, Bottom);
			BedHigh = FMath::Max(BedHigh, Bottom);

			if (Pixel.R.GetFloat() > 0.f)
			{
				WetBedLow = FMath::Min(WetBedLow, Bottom);
				WetBedHigh = FMath::Max(WetBedHigh, Bottom);
			}
		}

		Out.Appendf(
			TEXT("    bottom contour Z %.1f..%.1f over the window, %.1f..%.1f under water\n"),
			BedLow, BedHigh, WetBedLow, WetBedHigh);

		// HOW VIOLENTLY THE FLOOR JUMPS AT A PUDDLE'S RIM.
		//
		// The material samples this picture bilinearly, so a wet texel's floor gets
		// blended against whatever its dry neighbour reports -- and for ground the
		// field has never had water on, that is Window.NearZ, the VIEWER'S OWN Z.
		// The surface is placed at floor plus depth, so every such pair injects a
		// height somewhere between the real floor and the player's chest, along the
		// entire boundary. A big number here is edge noise you can see.
		// THIS SHEET'S PICTURE, not the ceiling on all of them. Each body sizes its
		// own grids now, so reading the cap here walked a 512-wide row through a
		// 256-wide array and took the process out.
		const int32 Side = FMath::Max(1, Sheet.PictureTexels);
		float WorstJump = 0.f;
		int32 RimPairs = 0;

		for (int32 Y = 0; Y < Side; ++Y)
		{
			for (int32 X = 0; X + 1 < Side; ++X)
			{
				const FFloat16Color& A = Sheet.Pixels[Y * Side + X];
				const FFloat16Color& B = Sheet.Pixels[Y * Side + X + 1];

				const bool bWetA = A.R.GetFloat() > 0.f;
				const bool bWetB = B.R.GetFloat() > 0.f;

				if (bWetA == bWetB)
				{
					continue;
				}

				++RimPairs;
				WorstJump = FMath::Max(WorstJump,
					FMath::Abs(A.G.GetFloat() - B.G.GetFloat()));
			}
		}

		Out.Appendf(
			TEXT("    %d wet/dry texel pairs at the rim, worst floor jump %.1fcm\n"),
			RimPairs, WorstJump);

		Out.Appendf(TEXT("    picture %d texels, %d wet, deepest %.2fcm, mean %.3fcm\n"),
			Sheet.Pixels.Num(), Wet, Deepest,
			Sheet.Pixels.Num() ? Total / Sheet.Pixels.Num() : 0.0);

		Out.Appendf(TEXT("    sampled from origin %s, near Z %.1f\n"),
			*Sheet.LastOrigin.ToString(), Sheet.LastNearZ);

		// WHAT THE SHADER WILL COMPUTE, worked out here with the same inputs.
		//
		// The material derives its UVs from the parameter COLLECTION while the
		// texture is uploaded from the window in UpdateSheet, and nothing has ever
		// checked that those two agree. If they do not, every texel the shader
		// reads is the wrong one -- the mask comes back dry over water, the
		// surface is transparent everywhere, and there is nothing on screen to
		// tell you why. Which is indistinguishable, from outside, from a material
		// that was never bound at all.
		if (Sheet.Material)
		{
			// FROM THE INSTANCE, which is where the material now reads them.
			float ShaderExtent = 0.f;
			FLinearColor ShaderOrigin = FLinearColor::Black;

			Sheet.Material->GetScalarParameterValue(
				FMaterialParameterInfo(SheetParams::FieldExtent), ShaderExtent);
			Sheet.Material->GetVectorParameterValue(
				FMaterialParameterInfo(SheetParams::FieldOrigin), ShaderOrigin);

			Out.Appendf(
				TEXT("    shader reads origin X=%.3f Y=%.3f extent %.0f -- off by (%.1f, %.1f)\n"),
				ShaderOrigin.R, ShaderOrigin.G, ShaderExtent,
				ShaderOrigin.R - Sheet.LastOrigin.X,
				ShaderOrigin.G - Sheet.LastOrigin.Y);
		}

		// AND WHERE THE VERTICES GO. The surface is placed by offsetting each one
		// onto FieldTexture.G + FieldTexture.R, so a bad value here does not draw
		// the water in the wrong place -- it throws the geometry off the screen
		// and draws nothing at all.
		float Lowest = TNumericLimits<float>::Max();
		float Highest = -TNumericLimits<float>::Max();
		int32 Broken = 0;

		// WET TEXELS ONLY, because those are the only vertices the offset moves.
		// Reporting the whole picture said "Z 0 to 302" and that was true and
		// useless -- it was measuring ground the field has no opinion about. What
		// matters is how far apart the vertices that actually carry water end up:
		// a real puddle is centimetres thick, so anything wider than the terrain
		// it lies on means the offset is tearing the sheet again.
		for (const FFloat16Color& Pixel : Sheet.Pixels)
		{
			if (Pixel.R.GetFloat() <= 0.f)
			{
				continue;
			}

			const float Surface = Pixel.R.GetFloat() + Pixel.G.GetFloat();

			if (!FMath::IsFinite(Surface))
			{
				++Broken;
				continue;
			}

			Lowest = FMath::Min(Lowest, Surface);
			Highest = FMath::Max(Highest, Surface);
		}

		Out.Appendf(
			TEXT("    wet surface sits between Z %.1f and %.1f, plane at %.1f (%d broken)\n"),
			Lowest, Highest,
			Sheet.Component ? Sheet.Component->GetComponentLocation().Z : 0.f, Broken);
	}

	return FString(Out.ToString());
}

#if !UE_BUILD_SHIPPING

// WHY THESE EXIST. The sheet is three transient objects and a dynamic material
// instance, none of which can be opened in the editor, so from outside the
// process a sheet drawing the wrong thing and a sheet handed the wrong thing are
// the same picture. Guessing between them cost an evening; printing them costs
// nothing.

namespace
{
	FTimerHandle GDebugSheetTimer;
	int32 GDebugSheetLeft = 0;

	void LogSheets(UWorld* World)
	{
		UARPGFluidPresentationSubsystem* Presentation =
			World ? World->GetSubsystem<UARPGFluidPresentationSubsystem>() : nullptr;

		if (!Presentation)
		{
			UE_LOG(LogARPGWorld, Warning,
				TEXT("no fluid presentation subsystem in this world"));
			return;
		}

		UE_LOG(LogARPGWorld, Display, TEXT("%s"), *Presentation->DescribeSheets());
	}
}

static FAutoConsoleCommandWithWorldAndArgs GARPGFluidDebugSheet(
	TEXT("ARPG.Fluid.DebugSheet"),
	TEXT("ARPG.Fluid.DebugSheet [seconds=1] -- print the sheet state, once a second."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (!World)
			{
				UE_LOG(LogARPGWorld, Warning, TEXT("no world to describe"));
				return;
			}

			LogSheets(World);

			// KEEPS PRINTING, because a sheet cannot exist in the frame the water
			// arrives -- it is created by the next Tick. Run this from -ExecCmds
			// alongside a pour and every command fires on the same frame, so the
			// honest answer is always "no sheets" and it means nothing at all.
			// Asking for a few seconds is the difference between measuring the
			// subsystem and measuring the order the console ran things in.
			const int32 Seconds = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 1;

			if (Seconds <= 1)
			{
				return;
			}

			GDebugSheetLeft = Seconds - 1;

			const TWeakObjectPtr<UWorld> Weak(World);

			World->GetTimerManager().SetTimer(GDebugSheetTimer,
				FTimerDelegate::CreateLambda([Weak]()
					{
						UWorld* Live = Weak.Get();

						if (!Live)
						{
							return;
						}

						LogSheets(Live);

						if (--GDebugSheetLeft <= 0)
						{
							Live->GetTimerManager().ClearTimer(GDebugSheetTimer);
						}
					}),
				1.f, true);
		}));

static FAutoConsoleCommandWithWorldAndArgs GARPGFluidShot(
	TEXT("ARPG.Fluid.Shot"),
	TEXT("ARPG.Fluid.Shot [seconds=3] -- take a screenshot once the water settles."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (!World)
			{
				return;
			}

			// SO THE PICTURE AND THE SCREEN CAN BE COMPARED. DescribeSheets says
			// what the shader was handed; this says what came out. Everything
			// between the two -- the UVs, the mask, whatever Substrate does to a
			// legacy opacity output -- is otherwise only inspectable by looking,
			// and -ExecCmds fires everything on frame one, before there is
			// anything to look at.
			const float Wait = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 3.f;

			FTimerHandle Handle;
			const TWeakObjectPtr<UWorld> Weak(World);

			World->GetTimerManager().SetTimer(Handle,
				FTimerDelegate::CreateLambda([Weak]()
					{
						if (!Weak.IsValid())
						{
							return;
						}

						// THE ENGINE API RATHER THAN THE CONSOLE COMMAND. HighResShot
						// through GEngine->Exec did nothing at all here -- no file, no
						// console response, no complaint -- and a screenshot that fails
						// silently is worse than none, because it reads as "the water
						// is invisible" rather than "nothing was captured".
						FScreenshotRequest::RequestScreenshot(
							TEXT("ARPGFluid"), false, false);

						UE_LOG(LogARPGWorld, Display,
							TEXT("screenshot requested into %s"),
							*FPaths::ScreenShotDir());
					}),
				FMath::Max(0.1f, Wait), false);
		}));

static FAutoConsoleCommandWithWorldAndArgs GARPGFluidPour(
	TEXT("ARPG.Fluid.Pour"),
	TEXT("ARPG.Fluid.Pour [volume=500000] [definition=0] -- pour at the viewer."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			UARPGFluidSurfaceSubsystem* Fluids =
				World ? World->GetSubsystem<UARPGFluidSurfaceSubsystem>() : nullptr;

			if (!Fluids)
			{
				UE_LOG(LogARPGWorld, Warning, TEXT("no fluid subsystem in this world"));
				return;
			}

			const double Volume = Args.Num() > 0 ? FCString::Atod(*Args[0]) : 500000.0;
			const int32 Which = Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 0;

			if (!Fluids->Definitions.IsValidIndex(Which))
			{
				UE_LOG(LogARPGWorld, Warning, TEXT("no fluid definition %d of %d"),
					Which, Fluids->Definitions.Num());
				return;
			}

			UARPGFluidDefinition* Definition = Fluids->Definitions[Which];

			FVector At = FVector::ZeroVector;

			if (const APlayerController* Viewer = World->GetFirstPlayerController())
			{
				if (const AActor* Pawn = Viewer->GetViewTarget())
				{
					At = Pawn->GetActorLocation();
				}
			}

			// THE FEET, NOT THE MIDDLE. A pawn's location is its capsule centre, and
			// pouring from there lands the body a metre up in the air on any floor
			// the probe has not been asked about yet.
			At.Z -= 90.f;

			const AARPGFluidRegion* Made = Fluids->ReturnFluid(
				FVector2D(At.X, At.Y), static_cast<float>(At.Z), Volume, Definition);

			UE_LOG(LogARPGWorld, Display,
				TEXT("poured %.0f of %s at %s -- %s"), Volume,
				Definition ? *Definition->GetName() : TEXT("?"),
				*At.ToCompactString(),
				Made ? TEXT("made a body") : TEXT("too little to be a body"));
		}));

#endif

void UARPGFluidPresentationSubsystem::ReleaseSheet(FARPGFluidSheet& Sheet)
{
	if (Sheet.Component)
	{
		Sheet.Component->DeactivateImmediate();
		Sheet.Component->DestroyComponent();
		Sheet.Component = nullptr;
	}

	if (Sheet.FieldTexture)
	{
		Sheet.FieldTexture->RemoveFromRoot();
		Sheet.FieldTexture = nullptr;
	}

	Sheet.NormalRT = nullptr;
	Sheet.VelocityRT = nullptr;
	Sheet.Material = nullptr;
	Sheet.Pixels.Empty();
}

void UARPGFluidPresentationSubsystem::ReleaseSheets()
{
	for (TPair<TObjectPtr<AARPGFluidRegion>, FARPGFluidSheet>& Pair : Sheets)
	{
		ReleaseSheet(Pair.Value);
	}

	Sheets.Reset();

	WindowCentre = FVector::ZeroVector;
	PublishedCentre = FVector::ZeroVector;
}
