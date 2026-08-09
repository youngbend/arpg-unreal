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
		FBox Bounds(ForceInit);
		bool bMeasured = MeasureLocalBounds(Target, Bounds);

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
				Bounds = VisualBounds;
				bMeasured = true;
			}
		}

		Instance->AttachToActor(Target, FAttachmentTransformRules::KeepRelativeTransform);

		const bool bSizesItself = Instance->Implements<UARPGVfxFittable>();

		if (bMeasured)
		{
			const FVector Size = Bounds.GetSize();
			const FVector Centre = Bounds.GetCenter();

			FVector Position = Centre;
			double Uniform = FMath::Max3(Size.X, Size.Y, Size.Z);

			switch (FitMode)
			{
			case EARPGStatusVfxFit::Base:
				Position = FVector(Centre.X, Centre.Y, Bounds.Min.Z);
				Uniform = BoundsFootprint(Bounds);
				break;

			case EARPGStatusVfxFit::Top:
				Position = FVector(Centre.X, Centre.Y, Bounds.Max.Z);
				Uniform = BoundsFootprint(Bounds);
				break;

			case EARPGStatusVfxFit::None:
				Position = FVector::ZeroVector;
				Uniform = 1.0;
				break;

			case EARPGStatusVfxFit::Bounds:
			default:
				break;
			}

			Instance->SetActorRelativeLocation(Position);

			if (!bSizesItself)
			{
				const double Scale = FMath::Max(
					static_cast<double>(MinFitScale), Uniform * ExtraScale);
				Instance->SetActorRelativeScale3D(FVector(Scale));
			}
		}
		else if (!bSizesItself)
		{
			// Nothing to measure -- an actor made of nothing but a scene root.
			// The authored size stands, adjusted only by the effect's own
			// multiplier, rather than collapsing to the minimum.
			Instance->SetActorRelativeScale3D(FVector(ExtraScale));
		}

		if (bSizesItself)
		{
			IARPGVfxFittable::Execute_Fit(Instance, bMeasured ? Bounds : FBox(ForceInit), Stacks);
			IARPGVfxFittable::Execute_SetStacks(Instance, Stacks);
		}
	}
}
