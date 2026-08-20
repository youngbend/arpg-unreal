// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGGeometryProbe.h"
#include "ARPGCombat.h"
#include "ARPGVfxFittable.h"
#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"

namespace ARPGGeometryProbe
{
	/** Below this a "fitted" effect on a tiny target collapses to nothing. */
	static constexpr float MinFitScale = 0.05f;

	bool MeasureLocalBounds(const AActor* Target, FBox& OutBounds, bool bVisualOnly)
	{
		if (!Target)
		{
			return false;
		}

		const FTransform ToLocal = Target->GetActorTransform().Inverse();

		FBox Accumulated(ForceInit);
		bool bMeasured = false;

		for (const UActorComponent* Component : Target->GetComponents())
		{
			const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
			if (!Primitive || !Primitive->IsRegistered())
			{
				continue;
			}

			// A MESH is what renders; a shape is what collides. That is the
			// distinction the visual-only question is really asking about, and
			// it is the closest thing UE has to Godot's VisualInstance3D /
			// CollisionShape3D split.
			if (bVisualOnly && !Primitive->IsA<UMeshComponent>())
			{
				continue;
			}

			// Recomputed against the component's transform RELATIVE to the
			// actor, rather than taking the world bounds and rotating the box
			// afterwards. Rotating an axis-aligned box only ever grows it, so
			// the cheap version reports a rotated sword as much fatter than it
			// is -- and every effect fitted to it comes out too big.
			const FTransform Relative = Primitive->GetComponentTransform() * ToLocal;
			const FBoxSphereBounds LocalBounds = Primitive->CalcBounds(Relative);

			Accumulated += LocalBounds.GetBox();
			bMeasured = true;
		}

		if (bMeasured)
		{
			OutBounds = Accumulated;
		}
		return bMeasured;
	}

	double BoundsFootprint(const FBox& Bounds)
	{
		const FVector Size = Bounds.GetSize();
		return FMath::Max(Size.X, Size.Y);
	}

	bool SolveVfxFit(AActor* Target, EARPGStatusVfxFit FitMode, float ExtraScale,
		FVector& OutRelativeLocation, double& OutScale, FBox& OutBounds)
	{
		OutRelativeLocation = FVector::ZeroVector;
		OutScale = ExtraScale;
		OutBounds = FBox(ForceInit);

		if (!Target)
		{
			return false;
		}

		bool bMeasured = MeasureLocalBounds(Target, OutBounds);

		// Top means "sitting on the surface", and for anything with a mesh that
		// is the MESH's top, not the union's. Water forces the distinction: its
		// collider deliberately stands above the waterline so a spell arriving
		// at the river enters it, and an arc placed on the union would float in
		// that headroom. A collider-only prop finds no mesh and keeps the union.
		if (FitMode == EARPGStatusVfxFit::Top)
		{
			FBox VisualBounds(ForceInit);
			if (MeasureLocalBounds(Target, VisualBounds, /*bVisualOnly=*/true))
			{
				OutBounds = VisualBounds;
				bMeasured = true;
			}
		}

		if (!bMeasured)
		{
			// Nothing to measure -- an actor made of nothing but a scene root. The
			// authored size stands, adjusted only by the effect's own multiplier,
			// rather than collapsing to the minimum.
			return false;
		}

		const FVector Size = OutBounds.GetSize();
		const FVector Centre = OutBounds.GetCenter();

		OutRelativeLocation = Centre;
		double Uniform = FMath::Max3(Size.X, Size.Y, Size.Z);

		switch (FitMode)
		{
		case EARPGStatusVfxFit::Base:
			OutRelativeLocation = FVector(Centre.X, Centre.Y, OutBounds.Min.Z);
			Uniform = BoundsFootprint(OutBounds);
			break;

		case EARPGStatusVfxFit::Top:
			OutRelativeLocation = FVector(Centre.X, Centre.Y, OutBounds.Max.Z);
			Uniform = BoundsFootprint(OutBounds);
			break;

		case EARPGStatusVfxFit::None:
			OutRelativeLocation = FVector::ZeroVector;
			Uniform = 1.0;
			break;

		case EARPGStatusVfxFit::Bounds:
		default:
			break;
		}

		OutScale = FMath::Max(static_cast<double>(MinFitScale), Uniform * ExtraScale);
		return true;
	}

	void FitVfxComponent(USceneComponent* Instance, AActor* Target,
		EARPGStatusVfxFit FitMode, float ExtraScale)
	{
		if (!Instance || !Target)
		{
			return;
		}

		// ALREADY ATTACHED, which is the one thing this does not have to arrange --
		// and it is also why the measure-before-attach ordering the actor path
		// worries about does not apply. A Niagara component's own bounds are not
		// part of its owner's until it is registered, and by then the sum is done.
		FVector Position;
		double Scale = ExtraScale;
		FBox Bounds(ForceInit);

		SolveVfxFit(Target, FitMode, ExtraScale, Position, Scale, Bounds);

		Instance->SetRelativeLocation(Position);
		Instance->SetRelativeScale3D(FVector(Scale));
	}

	void FitVfxToTarget(AActor* Instance, AActor* Target, EARPGStatusVfxFit FitMode,
		float ExtraScale, int32 Stacks)
	{
		if (!Instance || !Target)
		{
			return;
		}

		// A VFX actor with no root cannot be attached, placed or scaled -- every
		// call below would silently no-op and the effect would sit at the world
		// origin at unit size, looking like a fitting bug rather than a missing
		// component. Say so once instead.
		if (!Instance->GetRootComponent())
		{
			UE_LOG(LogARPGCombat, Warning,
				TEXT("'%s' has no root component, so it cannot be fitted to '%s'. ")
				TEXT("Give the VFX actor a scene root."),
				*GetNameSafe(Instance), *GetNameSafe(Target));
			return;
		}

		// Measured BEFORE attaching -- see the header.
		FVector Position;
		double Scale = ExtraScale;
		FBox Bounds(ForceInit);

		const bool bMeasured = SolveVfxFit(Target, FitMode, ExtraScale, Position, Scale, Bounds);

		Instance->AttachToActor(Target, FAttachmentTransformRules::KeepRelativeTransform);

		const bool bSizesItself = Instance->Implements<UARPGVfxFittable>();

		if (bMeasured)
		{
			Instance->SetActorRelativeLocation(Position);
		}

		if (!bSizesItself)
		{
			Instance->SetActorRelativeScale3D(FVector(Scale));
		}

		if (bSizesItself)
		{
			IARPGVfxFittable::Execute_Fit(Instance, bMeasured ? Bounds : FBox(ForceInit), Stacks);
			IARPGVfxFittable::Execute_SetStacks(Instance, Stacks);
		}
	}
}
