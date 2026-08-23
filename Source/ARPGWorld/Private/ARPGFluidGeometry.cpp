// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGFluidGeometry.h"
#include "ConstrainedDelaunay2.h"
#include "Components/DynamicMeshComponent.h"
#include "Curve/GeneralPolygon2.h"
#include "Curve/PolygonIntersectionUtils.h"
#include "Curve/PolygonOffsetUtils.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/MeshNormals.h"

namespace
{
	using namespace UE::Geometry;

	/** A ring as the library's polygon type. Empty when degenerate. */
	bool ToGeneralPolygon(const TArray<FVector2D>& Ring, FGeneralPolygon2d& Out)
	{
		if (Ring.Num() < 3)
		{
			return false;
		}

		FPolygon2d Outer;
		for (const FVector2D& Point : Ring)
		{
			Outer.AppendVertex(FVector2d(Point.X, Point.Y));
		}

		// The library requires a counter-clockwise outer boundary; a ring handed
		// back from a previous operation may be either.
		if (Outer.IsClockwise())
		{
			Outer.Reverse();
		}

		Out = FGeneralPolygon2d(Outer);
		return true;
	}

	TArray<FVector2D> FromPolygon(const FPolygon2d& Polygon)
	{
		TArray<FVector2D> Ring;
		Ring.Reserve(Polygon.VertexCount());

		for (int32 Index = 0; Index < Polygon.VertexCount(); ++Index)
		{
			const FVector2d& Vertex = Polygon[Index];
			Ring.Add(FVector2D(Vertex.X, Vertex.Y));
		}

		return Ring;
	}

	/**
	 * The largest outer ring in a result, discarding islands.
	 *
	 * "Largest" by area rather than vertex count: a boolean op can hand back a
	 * many-vertex sliver alongside the real body, and taking the wrong one would
	 * silently replace a pool with its own fringe.
	 */
	TArray<FVector2D> LargestOuterRing(const TArray<FGeneralPolygon2d>& Result)
	{
		const FPolygon2d* Best = nullptr;
		double BestArea = 0.0;

		for (const FGeneralPolygon2d& Polygon : Result)
		{
			const double Area = FMath::Abs(Polygon.GetOuter().SignedArea());
			if (Area > BestArea)
			{
				BestArea = Area;
				Best = &Polygon.GetOuter();
			}
		}

		return Best ? FromPolygon(*Best) : TArray<FVector2D>();
	}
}

namespace ARPGFluidGeometry
{

double PolygonArea(const TArray<FVector2D>& Ring)
{
	if (Ring.Num() < 3)
	{
		return 0.0;
	}

	double Twice = 0.0;
	for (int32 Index = 0; Index < Ring.Num(); ++Index)
	{
		const FVector2D& Current = Ring[Index];
		const FVector2D& Next = Ring[(Index + 1) % Ring.Num()];
		Twice += Current.X * Next.Y - Next.X * Current.Y;
	}

	// Absolute, so it does not matter whether this ring came back as a hole or
	// an outline.
	return FMath::Abs(Twice) * 0.5;
}

double PolygonPerimeter(const TArray<FVector2D>& Ring)
{
	if (Ring.Num() < 2)
	{
		return 0.0;
	}

	double Total = 0.0;
	for (int32 Index = 0; Index < Ring.Num(); ++Index)
	{
		Total += FVector2D::Distance(Ring[Index], Ring[(Index + 1) % Ring.Num()]);
	}
	return Total;
}

FVector2D PolygonCentroid(const TArray<FVector2D>& Ring)
{
	if (Ring.Num() == 0)
	{
		return FVector2D::ZeroVector;
	}

	double Twice = 0.0;
	FVector2D Accumulated = FVector2D::ZeroVector;

	for (int32 Index = 0; Index < Ring.Num(); ++Index)
	{
		const FVector2D& Current = Ring[Index];
		const FVector2D& Next = Ring[(Index + 1) % Ring.Num()];
		const double Cross = Current.X * Next.Y - Next.X * Current.Y;

		Twice += Cross;
		Accumulated += (Current + Next) * Cross;
	}

	if (FMath::IsNearlyZero(Twice))
	{
		// Degenerate -- a line, or coincident points. The vertex average is
		// still a usable anchor, and callers always need one.
		FVector2D Sum = FVector2D::ZeroVector;
		for (const FVector2D& Point : Ring)
		{
			Sum += Point;
		}
		return Sum / Ring.Num();
	}

	return Accumulated / (3.0 * Twice);
}

bool PolygonContains(const TArray<FVector2D>& Ring, const FVector2D& Point)
{
	if (Ring.Num() < 3)
	{
		return false;
	}

	bool bInside = false;
	for (int32 Index = 0, Previous = Ring.Num() - 1; Index < Ring.Num(); Previous = Index++)
	{
		const FVector2D& A = Ring[Index];
		const FVector2D& B = Ring[Previous];

		if (((A.Y > Point.Y) != (B.Y > Point.Y))
			&& (Point.X < (B.X - A.X) * (Point.Y - A.Y) / (B.Y - A.Y) + A.X))
		{
			bInside = !bInside;
		}
	}

	return bInside;
}

double PolygonSignedDistance(const TArray<FVector2D>& Ring, const FVector2D& Point)
{
	if (Ring.Num() < 3)
	{
		return TNumericLimits<double>::Max();
	}

	double Nearest = TNumericLimits<double>::Max();

	for (int32 Index = 0, Previous = Ring.Num() - 1; Index < Ring.Num(); Previous = Index++)
	{
		const FVector2D& A = Ring[Previous];
		const FVector2D& B = Ring[Index];

		// TO THE SEGMENT, not to the infinite line and not to the nearer endpoint.
		// A ring's vertices are metres apart on a long river bank and centimetres
		// apart where a spell clipped it, and a vertex-only distance would report
		// the first as far outside a point sitting right against it.
		const FVector2D Along = B - A;
		const double LengthSq = Along.SizeSquared();

		const double T = LengthSq > UE_DOUBLE_SMALL_NUMBER
			? FMath::Clamp(FVector2D::DotProduct(Point - A, Along) / LengthSq, 0.0, 1.0)
			: 0.0;

		Nearest = FMath::Min(Nearest, FVector2D::Distance(Point, A + Along * T));
	}

	return PolygonContains(Ring, Point) ? -Nearest : Nearest;
}

FBox2D PolygonBounds(const TArray<FVector2D>& Ring)
{
	FBox2D Bounds(ForceInit);
	for (const FVector2D& Point : Ring)
	{
		Bounds += Point;
	}
	return Bounds;
}

TArray<FVector2D> MakeCircle(const FVector2D& Centre, double Radius, int32 Segments)
{
	TArray<FVector2D> Ring;

	const int32 Count = FMath::Max(3, Segments);
	Ring.Reserve(Count);

	for (int32 Index = 0; Index < Count; ++Index)
	{
		const double Angle = 2.0 * PI * Index / Count;
		Ring.Add(Centre + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius);
	}

	return Ring;
}

TArray<FVector2D> MakeStadium(const FVector2D& A, const FVector2D& B, double Radius, int32 Segments)
{
	const FVector2D Along = B - A;
	if (Along.IsNearlyZero())
	{
		return MakeCircle(A, Radius, Segments);
	}

	const FVector2D Direction = Along.GetSafeNormal();
	const FVector2D Normal(-Direction.Y, Direction.X);

	const int32 HalfCount = FMath::Max(2, Segments / 2);
	TArray<FVector2D> Ring;
	Ring.Reserve(HalfCount * 2 + 2);

	// The cap at B, swinging from one side to the other, then the cap at A --
	// which traces the whole outline in one consistent winding.
	const double StartAngle = FMath::Atan2(Normal.Y, Normal.X);

	for (int32 Index = 0; Index <= HalfCount; ++Index)
	{
		const double Angle = StartAngle - PI * Index / HalfCount;
		Ring.Add(B + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius);
	}

	for (int32 Index = 0; Index <= HalfCount; ++Index)
	{
		const double Angle = StartAngle + PI - PI * Index / HalfCount;
		Ring.Add(A + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius);
	}

	return Ring;
}

TArray<FVector2D> MergeRings(const TArray<FVector2D>& A, const TArray<FVector2D>& B)
{
	FGeneralPolygon2d PolyA;
	FGeneralPolygon2d PolyB;

	const bool bHasA = ToGeneralPolygon(A, PolyA);
	const bool bHasB = ToGeneralPolygon(B, PolyB);

	if (!bHasA)
	{
		return bHasB ? B : TArray<FVector2D>();
	}
	if (!bHasB)
	{
		return A;
	}

	TArray<FGeneralPolygon2d> Inputs = { PolyA, PolyB };
	TArray<FGeneralPolygon2d> Result;

	if (!UE::Geometry::PolygonsUnion(Inputs, Result, /*bCopyInputOnFailure=*/true))
	{
		// Degenerate input. Keeping A is the conservative answer: a failed merge
		// should not delete the pool that already existed.
		return A;
	}

	return LargestOuterRing(Result);
}

TArray<FVector2D> OffsetRing(const TArray<FVector2D>& Ring, double Offset)
{
	FGeneralPolygon2d Polygon;
	if (!ToGeneralPolygon(Ring, Polygon))
	{
		return TArray<FVector2D>();
	}

	TArray<FGeneralPolygon2d> Inputs = { Polygon };
	TArray<FGeneralPolygon2d> Result;

	// Square joins rather than round: a puddle's edge is not a circular arc, and
	// round joins add vertices on every single offset -- which is how an
	// evaporating pool's outline grows without bound over a long session.
	if (!UE::Geometry::PolygonsOffset(Offset, Inputs, Result, /*bCopyInputOnFailure=*/false,
			/*MiterLimit=*/2.0, UE::Geometry::EPolygonOffsetJoinType::Square))
	{
		// Eroded to nothing. That is a real outcome, not a failure -- a puddle
		// that has fully evaporated -- so return empty rather than the input.
		return TArray<FVector2D>();
	}

	return LargestOuterRing(Result);
}

TArray<FVector2D> IntersectRings(const TArray<FVector2D>& A, const TArray<FVector2D>& B)
{
	FGeneralPolygon2d PolyA;
	FGeneralPolygon2d PolyB;

	if (!ToGeneralPolygon(A, PolyA) || !ToGeneralPolygon(B, PolyB))
	{
		return TArray<FVector2D>();
	}

	TArray<FGeneralPolygon2d> SubjectArray = { PolyA };
	TArray<FGeneralPolygon2d> ClipArray = { PolyB };
	TArray<FGeneralPolygon2d> Result;

	if (!UE::Geometry::PolygonsIntersection(SubjectArray, ClipArray, Result))
	{
		return TArray<FVector2D>();
	}

	return LargestOuterRing(Result);
}

TArray<FVector2D> SubtractRings(const TArray<FVector2D>& A, const TArray<FVector2D>& B)
{
	FGeneralPolygon2d PolyA;
	FGeneralPolygon2d PolyB;

	if (!ToGeneralPolygon(A, PolyA))
	{
		return TArray<FVector2D>();
	}
	if (!ToGeneralPolygon(B, PolyB))
	{
		return A;
	}

	TArray<FGeneralPolygon2d> PositiveArray = { PolyA };
	TArray<FGeneralPolygon2d> NegativeArray = { PolyB };
	TArray<FGeneralPolygon2d> Result;

	if (!UE::Geometry::PolygonsDifference(PositiveArray, NegativeArray, Result))
	{
		return A;
	}

	return LargestOuterRing(Result);
}

void IntersectWithHoles(const TArray<FVector2D>& A, const TArray<FVector2D>& B,
	TArray<FVector2D>& OutRing, TArray<TArray<FVector2D>>& OutHoles)
{
	OutRing.Reset();
	OutHoles.Reset();

	FGeneralPolygon2d PolyA;
	FGeneralPolygon2d PolyB;

	if (!ToGeneralPolygon(A, PolyA) || !ToGeneralPolygon(B, PolyB))
	{
		return;
	}

	TArray<FGeneralPolygon2d> SubjectArray = { PolyA };
	TArray<FGeneralPolygon2d> ClipArray = { PolyB };
	TArray<FGeneralPolygon2d> Result;

	if (!UE::Geometry::PolygonsIntersection(SubjectArray, ClipArray, Result))
	{
		return;
	}

	const FGeneralPolygon2d* Best = nullptr;
	double BestArea = 0.0;

	for (const FGeneralPolygon2d& Polygon : Result)
	{
		const double Area = FMath::Abs(Polygon.GetOuter().SignedArea());
		if (Area > BestArea)
		{
			BestArea = Area;
			Best = &Polygon;
		}
	}

	if (!Best)
	{
		return;
	}

	OutRing = FromPolygon(Best->GetOuter());

	// Kept SEPARATE rather than bridged -- see the header. Bridging leaves a
	// zero-width slit that the offsetter rounds into arcs, and eroding that
	// compounds until a melting floe stalls the frame.
	for (const FPolygon2d& Hole : Best->GetHoles())
	{
		OutHoles.Add(FromPolygon(Hole));
	}
}

void SubtractWithHoles(const TArray<FVector2D>& A, const TArray<FVector2D>& B,
	TArray<FVector2D>& OutRing, TArray<TArray<FVector2D>>& OutHoles)
{
	OutRing.Reset();
	OutHoles.Reset();

	FGeneralPolygon2d PolyA;
	FGeneralPolygon2d PolyB;

	if (!ToGeneralPolygon(A, PolyA))
	{
		return;
	}

	if (!ToGeneralPolygon(B, PolyB))
	{
		OutRing = A;
		return;
	}

	TArray<FGeneralPolygon2d> PositiveArray = { PolyA };
	TArray<FGeneralPolygon2d> NegativeArray = { PolyB };
	TArray<FGeneralPolygon2d> Result;

	if (!UE::Geometry::PolygonsDifference(PositiveArray, NegativeArray, Result))
	{
		OutRing = A;
		return;
	}

	const FGeneralPolygon2d* Best = nullptr;
	double BestArea = 0.0;

	for (const FGeneralPolygon2d& Polygon : Result)
	{
		const double Area = FMath::Abs(Polygon.GetOuter().SignedArea());
		if (Area > BestArea)
		{
			BestArea = Area;
			Best = &Polygon;
		}
	}

	if (!Best)
	{
		return;
	}

	OutRing = FromPolygon(Best->GetOuter());

	for (const FPolygon2d& Hole : Best->GetHoles())
	{
		OutHoles.Add(FromPolygon(Hole));
	}
}

namespace
{
	/**
	 * Scales a ring about its centroid to enclose a target area, either way.
	 *
	 * Area scales with the SQUARE of a uniform scale, so this is one square root
	 * rather than an iterative solve. Uniform rather than an offset because an
	 * offset erodes thin necks away entirely and changes the shape -- wrong when
	 * the point is only that fluid was added or taken.
	 */
	TArray<FVector2D> ScaleToArea(const TArray<FVector2D>& Ring, double TargetArea)
	{
		const double Scale = FMath::Sqrt(TargetArea / PolygonArea(Ring));
		const FVector2D Centre = PolygonCentroid(Ring);

		TArray<FVector2D> Scaled;
		Scaled.Reserve(Ring.Num());

		for (const FVector2D& Point : Ring)
		{
			Scaled.Add(Centre + (Point - Centre) * Scale);
		}

		return Scaled;
	}
}

TArray<FVector2D> ShrinkToArea(const TArray<FVector2D>& Ring, double TargetArea)
{
	const double Area = PolygonArea(Ring);
	if (Area <= 0.0 || TargetArea >= Area)
	{
		return Ring;
	}

	if (TargetArea <= 0.0)
	{
		return TArray<FVector2D>();
	}

	return ScaleToArea(Ring, TargetArea);
}

TArray<FVector2D> GrowToArea(const TArray<FVector2D>& Ring, double TargetArea)
{
	const double Area = PolygonArea(Ring);

	// Refuses to shrink, so the pair keep one-way contracts.
	if (Area <= 0.0 || TargetArea <= Area)
	{
		return Ring;
	}

	return ScaleToArea(Ring, TargetArea);
}

// ---------------------------------------------------------------------------
// Meshing
// ---------------------------------------------------------------------------

namespace
{
	/**
	 * One tile of surface texture per metre of world.
	 *
	 * Anchored to WORLD position rather than the mesh's own space -- see
	 * BuildSlabMesh. The number itself only decides how big the material's detail
	 * reads; it is here rather than on the definition because changing it per
	 * fluid buys nothing a material's own tiling cannot do better.
	 */
	constexpr double SurfaceUVScale = 100.0;

	/** Appends a vertex carrying its world-anchored UV. */
	int32 AppendSurfaceVertex(UE::Geometry::FDynamicMesh3& Mesh, const FVector2D& Origin,
		double LocalX, double LocalY, double Z)
	{
		const int32 Index = Mesh.AppendVertex(FVector3d(LocalX, LocalY, Z));

		Mesh.SetVertexUV(Index, FVector2f(
			static_cast<float>((LocalX + Origin.X) / SurfaceUVScale),
			static_cast<float>((LocalY + Origin.Y) / SurfaceUVScale)));

		return Index;
	}

	/**
	 * The wall under one ring, joining its top edge to its bottom.
	 *
	 * Wound so the face points AWAY from the body's interior for an outer ring
	 * given counter-clockwise, and into the gap for a hole given clockwise --
	 * which is the winding each already has by the time it gets here.
	 *
	 * WHICH WAY A TRIANGLE FACES IS NOT THE RIGHT-HAND RULE HERE. Unreal's
	 * coordinate system is left-handed, so FDynamicMesh3 takes a triangle's
	 * normal as (C - A) x (B - A) -- the reverse of the cross product every
	 * other library uses. See VectorUtil::Normal, which says so in a comment.
	 * Wound the other way every face in the body points at its own interior:
	 * the top is a backface you see straight through to the underside, and the
	 * complex collision cooked from it has no upward surface to land on.
	 */
	void AppendWall(UE::Geometry::FDynamicMesh3& Mesh, const TArray<FVector2D>& Ring,
		const FVector2D& Origin, double BottomZ, double TopZ,
		const ARPGFluidGeometry::FBedSampler& Bed,
		const ARPGFluidGeometry::FBedSampler& TopRelief = ARPGFluidGeometry::FBedSampler())
	{
		const int32 Count = Ring.Num();

		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FVector2D& A = Ring[Index];
			const FVector2D& B = Ring[(Index + 1) % Count];

			// THE WALL FOLLOWS THE FLOOR TOO. Lifting only the cap would leave the
			// rim of a puddle on a ramp joined to a skirt that stayed level, which
			// is a worse artefact than the flat pool it replaced.
			const double LiftA = Bed ? Bed(A + Origin) : 0.0;
			const double LiftB = Bed ? Bed(B + Origin) : 0.0;

			// Fresh vertices per quad rather than shared with the cap, so the lip
			// of the slab is a HARD edge. Sharing them would average the cap's
			// upward normal into the wall's sideways one and round the whole thing
			// off, which on a 20cm puddle reads as a blob rather than as water
			// with an edge.
			// THE RIM FOLLOWS THE DISH. A bowl cut near the edge of a slab lowers
			// the top surface right up to the outline, and a wall that ignored it
			// would stand proud of the cap it is supposed to join -- a crack you
			// can see through, exactly where the spell landed.
			const double ReliefA = TopRelief ? TopRelief(A + Origin) : 0.0;
			const double ReliefB = TopRelief ? TopRelief(B + Origin) : 0.0;

			const int32 TopA = AppendSurfaceVertex(Mesh, Origin, A.X, A.Y, TopZ + LiftA + ReliefA);
			const int32 TopB = AppendSurfaceVertex(Mesh, Origin, B.X, B.Y, TopZ + LiftB + ReliefB);
			const int32 BottomA = AppendSurfaceVertex(Mesh, Origin, A.X, A.Y, BottomZ + LiftA);
			const int32 BottomB = AppendSurfaceVertex(Mesh, Origin, B.X, B.Y, BottomZ + LiftB);

			Mesh.AppendTriangle(TopA, BottomB, BottomA);
			Mesh.AppendTriangle(TopA, TopB, BottomB);
		}
	}
}

void BuildSlabMesh(UDynamicMeshComponent* Component, const TArray<FVector2D>& Ring,
	const TArray<FVector2D>& Hole, const FVector2D& Origin, double BottomZ, double TopZ,
	const FBedSampler& Bed, double DetailSpacing, const FBedSampler& TopRelief)
{
	using namespace UE::Geometry;

	if (!Component)
	{
		return;
	}

	FDynamicMesh3 Mesh;
	Mesh.EnableVertexNormals(FVector3f::UnitZ());
	Mesh.EnableVertexUVs(FVector2f::Zero());

	// INTO THE COMPONENT'S OWN SPACE FIRST, which is what Origin is for.
	//
	// THE MESH USED TO BE BUILT IN WORLD COORDINATES AND THEN DRAWN AS LOCAL
	// ONES. The actor sits at Origin, so every vertex came out displaced by
	// exactly however far the body was from the world origin: a puddle in the
	// middle of a stage looked close enough to right, and one out at the edge was
	// drawn at twice its distance and left the map. The caller subtracts its own
	// origin from everything it hands in; these two paths simply disagreed about
	// whose space the ring was in.
	TArray<FVector2D> LocalRing;
	LocalRing.Reserve(Ring.Num());
	for (const FVector2D& Point : Ring)
	{
		LocalRing.Add(Point - Origin);
	}

	TArray<FVector2D> LocalHole;
	LocalHole.Reserve(Hole.Num());
	for (const FVector2D& Point : Hole)
	{
		LocalHole.Add(Point - Origin);
	}

	FGeneralPolygon2d Polygon;
	if (!ToGeneralPolygon(LocalRing, Polygon))
	{
		// Eroded away to nothing. An EMPTY mesh rather than an early return: the
		// component is still showing the last shape it was given, and leaving it
		// there is how a fully evaporated puddle stays visible forever.
		Component->SetMesh(MoveTemp(Mesh));
		Component->NotifyMeshUpdated();
		return;
	}

	// The hole has to wind against the outline for the fill rule to read it as a
	// gap rather than as a second body sitting inside the first.
	TArray<FVector2D> WoundHole;
	if (LocalHole.Num() >= 3)
	{
		FPolygon2d HolePolygon;
		for (const FVector2D& Point : LocalHole)
		{
			HolePolygon.AppendVertex(FVector2d(Point.X, Point.Y));
		}

		if (!HolePolygon.IsClockwise())
		{
			HolePolygon.Reverse();
		}

		// Containment and orientation both unchecked: IntersectWithHoles already
		// produced this as a hole of this outline, and a rejected hole would
		// silently mesh over a gap someone can fall through.
		Polygon.AddHole(HolePolygon, /*bCheckContainment=*/false, /*bCheckOrientation=*/false);

		WoundHole = FromPolygon(HolePolygon);
	}

	// A CONSTRAINED DELAUNAY rather than ear clipping, because these outlines are
	// arbitrarily concave -- a pool that three spells have landed in and an ice
	// shard has cut across -- and a solid's may have a gap in the middle. Both are
	// exactly what the constrained triangulator is for.
	FConstrainedDelaunay2d Triangulator;
	Triangulator.FillRule = FConstrainedDelaunay2d::EFillRule::Positive;
	Triangulator.Add(Polygon);

	// INTERIOR POINTS ON A GRID, so the cap has somewhere to bend.
	//
	// A constrained triangulation puts vertices only where the OUTLINE has them,
	// which for a puddle is a ring of sixteen and nothing in the middle. That is
	// exactly right for a flat body and useless for one following the floor: the
	// whole interior would be a handful of triangles spanning metres, and any
	// floor that is not a single plane would be cut straight across. A plane it
	// still reproduces exactly -- linear interpolation of a linear surface --
	// which is why this stays off until a bed sampler asks for it.
	if ((Bed || TopRelief) && DetailSpacing > 0.0)
	{
		const FBox2D Extent = PolygonBounds(LocalRing);

		for (double X = Extent.Min.X; X <= Extent.Max.X; X += DetailSpacing)
		{
			for (double Y = Extent.Min.Y; Y <= Extent.Max.Y; Y += DetailSpacing)
			{
				const FVector2D Point(X, Y);

				// Inside the body and clear of its edge. A Steiner point sitting
				// on the outline fights the constraint edge it duplicates, and the
				// triangulator is entitled to refuse the whole polygon over it.
				if (PolygonContains(LocalRing, Point)
					&& (LocalHole.Num() < 3 || !PolygonContains(LocalHole, Point)))
				{
					Triangulator.Vertices.Add(FVector2d(X, Y));
				}
			}
		}
	}

	if (!Triangulator.Triangulate() || Triangulator.Triangles.Num() == 0)
	{
		Component->SetMesh(FDynamicMesh3());
		Component->NotifyMeshUpdated();
		return;
	}

	const int32 CapCount = Triangulator.Vertices.Num();
	const bool bHasDepth = TopZ - BottomZ > UE_DOUBLE_SMALL_NUMBER;

	// LIFTED ONTO THE FLOOR, vertex by vertex. Both caps take the same lift, so
	// the body keeps its thickness everywhere and a puddle on a ramp is a sheet
	// down the ramp rather than a wedge that thins out at the top of it.
	for (const FVector2d& Vertex : Triangulator.Vertices)
	{
		const FVector2D At = FVector2D(Vertex.X, Vertex.Y) + Origin;

		// THE TOP CARRIES BOTH: whatever moved the body, and whatever was cut into
		// its upper surface alone.
		const double Lift = Bed ? Bed(At) : 0.0;
		const double Relief = TopRelief ? TopRelief(At) : 0.0;

		AppendSurfaceVertex(Mesh, Origin, Vertex.X, Vertex.Y, TopZ + Lift + Relief);
	}

	if (bHasDepth)
	{
		for (const FVector2d& Vertex : Triangulator.Vertices)
		{
			const double Lift = Bed ? Bed(FVector2D(Vertex.X, Vertex.Y) + Origin) : 0.0;
			AppendSurfaceVertex(Mesh, Origin, Vertex.X, Vertex.Y, BottomZ + Lift);
		}
	}

	for (const FIndex3i& Triangle : Triangulator.Triangles)
	{
		// FROM THE TRIANGLE'S OWN SIGNED AREA, rather than from an assumption about
		// which way the triangulator wound it.
		//
		// THIS USED TO ASSUME COUNTER-CLOCKWISE AND GET THE OPPOSITE. Under
		// Unreal's left-handed rule -- see AppendWall -- a face points UP when it
		// is clockwise in XY, so reversing a counter-clockwise triangle is right
		// and reversing a clockwise one turns the body inside out. The
		// constrained triangulator does not promise either, and what it actually
		// hands back here left every cap facing into the body: a trace from above
		// missed the top entirely, passed through, and stopped on the underside
		// from the inside, which is what falling into a slab you jumped onto looks
		// like. Walls were always right, because AppendWall winds its own quads
		// and never asked.
		//
		// Nothing was watching it happen. Pools are thin and drawn with a material
		// that does not care, and the only body anyone traced against was a slab --
		// which had its own mesher until the heightfield went, and that one built
		// its corners explicitly rather than trusting anything.
		const FVector2d& PA = Triangulator.Vertices[Triangle.A];
		const FVector2d& PB = Triangulator.Vertices[Triangle.B];
		const FVector2d& PC = Triangulator.Vertices[Triangle.C];

		const double Twice = (PB.X - PA.X) * (PC.Y - PA.Y) - (PC.X - PA.X) * (PB.Y - PA.Y);
		const bool bCounterClockwise = Twice > 0.0;

		// The cap everything is meant to be seen on and stood on.
		if (bCounterClockwise)
		{
			Mesh.AppendTriangle(Triangle.A, Triangle.C, Triangle.B);
		}
		else
		{
			Mesh.AppendTriangle(Triangle.A, Triangle.B, Triangle.C);
		}

		if (bHasDepth)
		{
			// And the underside is the other way about, so it faces DOWN. A slab
			// whose floor pointed up is invisible from below and lets you see into
			// it through the walls.
			if (bCounterClockwise)
			{
				Mesh.AppendTriangle(Triangle.A + CapCount, Triangle.B + CapCount,
					Triangle.C + CapCount);
			}
			else
			{
				Mesh.AppendTriangle(Triangle.A + CapCount, Triangle.C + CapCount,
					Triangle.B + CapCount);
			}
		}
	}

	if (bHasDepth)
	{
		AppendWall(Mesh, FromPolygon(Polygon.GetOuter()), Origin, BottomZ, TopZ, Bed, TopRelief);

		if (WoundHole.Num() >= 3)
		{
			AppendWall(Mesh, WoundHole, Origin, BottomZ, TopZ, Bed, TopRelief);
		}
	}

	FMeshNormals::QuickComputeVertexNormals(Mesh);

	Component->SetMesh(MoveTemp(Mesh));
	Component->NotifyMeshUpdated();
}

} // namespace ARPGFluidGeometry
