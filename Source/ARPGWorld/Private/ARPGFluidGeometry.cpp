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
		const ARPGFluidGeometry::FBedSampler& Bed)
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
			const int32 TopA = AppendSurfaceVertex(Mesh, Origin, A.X, A.Y, TopZ + LiftA);
			const int32 TopB = AppendSurfaceVertex(Mesh, Origin, B.X, B.Y, TopZ + LiftB);
			const int32 BottomA = AppendSurfaceVertex(Mesh, Origin, A.X, A.Y, BottomZ + LiftA);
			const int32 BottomB = AppendSurfaceVertex(Mesh, Origin, B.X, B.Y, BottomZ + LiftB);

			Mesh.AppendTriangle(TopA, BottomB, BottomA);
			Mesh.AppendTriangle(TopA, TopB, BottomB);
		}
	}
}

void BuildSlabMesh(UDynamicMeshComponent* Component, const TArray<FVector2D>& Ring,
	const TArray<FVector2D>& Hole, const FVector2D& Origin, double BottomZ, double TopZ,
	const FBedSampler& Bed, double DetailSpacing)
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
	// drawn at twice its distance and left the map. BuildFieldMesh has always
	// subtracted this -- see the Centre it takes off every cell -- and the two
	// paths simply disagreed about whose space the ring was in.
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
	if (Bed && DetailSpacing > 0.0)
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
		const double Lift = Bed ? Bed(FVector2D(Vertex.X, Vertex.Y) + Origin) : 0.0;
		AppendSurfaceVertex(Mesh, Origin, Vertex.X, Vertex.Y, TopZ + Lift);
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
		// REVERSED FROM THE TRIANGULATOR'S OWN WINDING, which is the usual
		// counter-clockwise. Under Unreal's left-handed rule -- see AppendWall --
		// that winding points a cap DOWN, so the surface everything is meant to
		// be seen and stood on faced into the body.
		Mesh.AppendTriangle(Triangle.A, Triangle.C, Triangle.B);

		if (bHasDepth)
		{
			// And the underside is the triangulator's winding untouched, so it
			// faces DOWN. A slab whose floor pointed up is invisible from below
			// and lets you see into it through the walls.
			Mesh.AppendTriangle(Triangle.A + CapCount, Triangle.B + CapCount,
				Triangle.C + CapCount);
		}
	}

	if (bHasDepth)
	{
		AppendWall(Mesh, FromPolygon(Polygon.GetOuter()), Origin, BottomZ, TopZ, Bed);

		if (WoundHole.Num() >= 3)
		{
			AppendWall(Mesh, WoundHole, Origin, BottomZ, TopZ, Bed);
		}
	}

	FMeshNormals::QuickComputeVertexNormals(Mesh);

	Component->SetMesh(MoveTemp(Mesh));
	Component->NotifyMeshUpdated();
}

namespace
{
	/**
	 * A repeatable wobble in [-0.5, 0.5) for one lattice corner.
	 *
	 * FROM THE CORNER'S INDEX AND NOTHING ELSE, which is the property that
	 * matters. The same corner has to come out displaced by the same amount on
	 * every rebuild or the whole surface swims sideways each time a spell lands
	 * on it, and it has to come out the same on a client as on the server or the
	 * rock you can see is not the rock you are standing on. An index satisfies
	 * both; a position does not, because a drifting floe's Origin moves under it.
	 */
	float CornerNoise(int32 X, int32 Y, uint32 Salt)
	{
		uint32 Hash = static_cast<uint32>(X) * 73856093u
			^ static_cast<uint32>(Y) * 19349663u
			^ (Salt * 83492791u);

		Hash ^= Hash >> 13;
		Hash *= 0x5bd1e995u;
		Hash ^= Hash >> 15;

		return (Hash & 0xFFFFFFu) / static_cast<float>(0x1000000) - 0.5f;
	}

	/**
	 * The field read as POINTS rather than as squares -- one sample at the centre
	 * of every cell, joined to its neighbours.
	 *
	 * THIS IS THE WHOLE TRICK, and it is a change of reading rather than a change
	 * to what is stored. A value per cell is a value per AREA: it says "this
	 * square is 30cm thick" and nothing whatever about what happens between this
	 * square and the next, so the only honest drawing of it is a box -- which is
	 * exactly the Minecraft slab this replaced. Treat the same number as a sample
	 * AT the centre and four of them become a patch of surface, and the patch
	 * bends.
	 *
	 * SAMPLED AT CELL CENTRES AND NOT AT THE CORNERS BETWEEN THEM, which is a
	 * distinction worth the paragraph. Corners are the more obvious choice and are
	 * what the first version of this used; averaging the four cells around each
	 * one gives a smoother surface, and a WRONG one -- the average blurs across a
	 * three-by-three stencil, so a one-cell pit comes out a third as deep as the
	 * field says it is and a slab's own TopAt no longer describes what the player
	 * is standing on. Sampling the centres reproduces every stored value exactly
	 * and interpolates only between them, so the drawn surface and the queried
	 * one are the same function -- see FARPGSolidField::TopAt, which is this
	 * evaluated at an arbitrary point.
	 *
	 * The samples live for the length of one rebuild. Cells are still what melts,
	 * what floats, and what replicates.
	 */
	struct FSurfaceLattice
	{
		int32 CountX = 0;
		int32 CountY = 0;

		/** Where each sample is, in the mesh's own space. Jittered if faceted. */
		TArray<FVector2D> At;

		/** Positive where there is material, negative where there is not. */
		TArray<float> Fill;

		TArray<double> Top;
		TArray<double> Bottom;

		int32 Index(int32 I, int32 J) const { return J * CountX + I; }
		int32 Num() const { return CountX * CountY; }
	};

	void BuildLattice(FSurfaceLattice& Out, const FARPGSolidField& Field,
		const FVector2D& Origin, const TArray<float>& Fill,
		const TArray<double>& BottomOf, const TArray<double>& TopOf,
		const FARPGSurfaceFacets& Facets)
	{
		Out.CountX = Field.CountX;
		Out.CountY = Field.CountY;

		const int32 Samples = Out.Num();
		Out.At.SetNumUninitialized(Samples);
		Out.Fill.SetNumUninitialized(Samples);
		Out.Top.SetNumUninitialized(Samples);
		Out.Bottom.SetNumUninitialized(Samples);

		for (int32 Y = 0; Y < Out.CountY; ++Y)
		{
			for (int32 X = 0; X < Out.CountX; ++X)
			{
				const int32 At = Out.Index(X, Y);

				// STRAIGHT THROUGH, unaveraged and unsmoothed. Whatever the caller
				// worked out for this cell is what the surface passes through.
				Out.Fill[At] = Fill[At];
				Out.Top[At] = TopOf[At];
				Out.Bottom[At] = BottomOf[At];

				FVector2D Where = Field.CentreOf(X, Y) - Origin;

				if (Facets.Spread > 0.f)
				{
					Where += FVector2D(CornerNoise(X, Y, 1u), CornerNoise(X, Y, 2u))
						* (Facets.Spread * Field.CellSize);
				}

				Out.At[At] = Where;

				if (Facets.Relief > 0.f)
				{
					// THE WHOLE COLUMN, top and bottom by the same amount, so the
					// cell keeps exactly the thickness the simulation gave it --
					// see FARPGSurfaceFacets::Relief. It is also what lets a film
					// lying on a broken surface be broken identically and stay in
					// contact with it, since both lattices hash the same sample.
					const double Lift = CornerNoise(X, Y, 3u) * Facets.Relief;

					Out.Top[At] += Lift;
					Out.Bottom[At] += Lift;
				}
			}
		}
	}

	/** One vertex of the shape a single quad contributes. */
	struct FCellVertex
	{
		FVector2D At = FVector2D::ZeroVector;
		double Top = 0.0;
		double Bottom = 0.0;

		/** Which of the quad's four sides this point lies on. */
		uint8 OnSides = 0;

		/** Its identity in the lattice, so neighbouring quads can share it. */
		int32 Key = INDEX_NONE;
	};

	/**
	 * One layer of the field as a surface: marching squares over the lattice, with
	 * the material's own top and bottom carried along.
	 *
	 * WHAT REPLACED THE BOX PER CELL, and the difference is the whole of what a
	 * slab looks like. The old rule drew each cell as an axis-aligned box and put
	 * a vertical face wherever one box stood proud of the next, which is a
	 * faithful drawing of a value-per-cell and a terrible drawing of a SURFACE:
	 * the smooth dish MeltBowl writes into the field came out as a ziggurat, and
	 * an outline that had been clipped to a fraction of a millimetre came out as
	 * a staircase with one step per cell.
	 *
	 * THREE THINGS FALL OUT OF JOINING THE SAMPLES UP.
	 *
	 *   THE SILHOUETTE GETS CUT WHERE THE MATERIAL ACTUALLY ENDS. Fill changes
	 *   sign somewhere along the line between two samples, and that somewhere is
	 *   a point, not a cell -- so the edge of the slab follows the polygon it
	 *   froze from rather than the grid it was stored on.
	 *
	 *   THE INTERIOR RISERS DISAPPEAR ENTIRELY. Two neighbouring quads share two
	 *   samples and therefore share their heights exactly, so the top is one
	 *   continuous surface with nothing to step over. Every vertical face that
	 *   survives is a real edge of the slab.
	 *
	 *   IT IS FEWER TRIANGLES, not more. A box is four cap triangles and up to
	 *   sixteen of side; a marching-squares quad is at most four and four, plus
	 *   two per contour segment on the handful the edge runs through.
	 *
	 * Spans are LOCAL Z in centimetres, indexed as the field is, and the caller
	 * has already decided what an empty cell's height should be -- see
	 * FARPGSolidField::SurfaceTopAt. Fill says where the material stops and is
	 * what the contour is cut from.
	 */
	void BuildCellLayer(UE::Geometry::FDynamicMesh3& Mesh, const FARPGSolidField& Field,
		const FVector2D& Origin, const TArray<float>& Fill,
		const TArray<double>& BottomOf, const TArray<double>& TopOf,
		const FARPGSurfaceFacets& Facets)
	{
		FSurfaceLattice Lattice;
		BuildLattice(Lattice, Field, Origin, Fill, BottomOf, TopOf, Facets);

		const int32 Samples = Lattice.Num();

		// FLAT SHADING IS JUST NOT SHARING. The normal solver averages the faces
		// around each vertex, so a vertex belonging to one triangle gets that
		// triangle's own normal and a surface built without sharing is faceted for
		// free -- see FARPGSurfaceFacets::bFlatShaded.
		const bool bShare = !Facets.bFlatShaded;

		// Sample points, then horizontal edge crossings, then vertical ones.
		TArray<int32> TopVertexOf;
		TArray<int32> BottomVertexOf;

		if (bShare)
		{
			TopVertexOf.Init(INDEX_NONE, Samples * 3);
			BottomVertexOf.Init(INDEX_NONE, Samples * 3);
		}

		auto Reuse = [&](TArray<int32>& Cache, int32 Key, double X, double Y, double Z) -> int32
		{
			if (bShare && Key != INDEX_NONE && Cache[Key] != INDEX_NONE)
			{
				return Cache[Key];
			}

			const int32 Vertex = AppendSurfaceVertex(Mesh, Origin, X, Y, Z);

			if (bShare && Key != INDEX_NONE)
			{
				Cache[Key] = Vertex;
			}

			return Vertex;
		};

		// WHERE THE CONTOUR CROSSES ONE LATTICE EDGE, computed from the lower of
		// the two sample indices every time.
		//
		// BOTH QUADS SHARING THE EDGE MUST GET BIT-IDENTICAL ANSWERS. They walk
		// their own boundaries in opposite directions along it, so interpolating
		// "from the sample I reached first" would give the two of them the same
		// point computed two ways -- and floating-point lerp is not symmetric, so
		// they would land microns apart and leave a crack down every silhouette.
		auto Crossing = [&](int32 SampleA, int32 SampleB, int32 Key, uint8 OnSides)
		{
			const int32 Low = FMath::Min(SampleA, SampleB);
			const int32 High = FMath::Max(SampleA, SampleB);

			const float FillLow = Lattice.Fill[Low];
			const float FillHigh = Lattice.Fill[High];

			const float Span = FillLow - FillHigh;
			const float T = FMath::Abs(Span) > UE_SMALL_NUMBER
				? FMath::Clamp(FillLow / Span, 0.f, 1.f)
				: 0.5f;

			FCellVertex Vertex;
			Vertex.At = FMath::Lerp(Lattice.At[Low], Lattice.At[High], T);
			Vertex.Top = FMath::Lerp(Lattice.Top[Low], Lattice.Top[High], T);
			Vertex.Bottom = FMath::Lerp(Lattice.Bottom[Low], Lattice.Bottom[High], T);
			Vertex.OnSides = OnSides;
			Vertex.Key = Key;

			return Vertex;
		};

		FCellVertex Poly[8];
		int32 TopIndex[8];
		int32 BottomIndex[8];

		// ONE FEWER QUAD THAN THERE ARE SAMPLES, in each axis, because a quad is
		// the gap BETWEEN four of them. The half-cell fringe outside the outermost
		// centres is not meshed and does not need to be: BuildFrom pads the grid by
		// a whole cell, so the slab's own edge always falls well inside it.
		for (int32 Y = 0; Y + 1 < Lattice.CountY; ++Y)
		{
			for (int32 X = 0; X + 1 < Lattice.CountX; ++X)
			{
				// COUNTER-CLOCKWISE IN XY, which is the winding everything below
				// assumes and the reverse of what points a face up -- see
				// AppendWall for why this coordinate system works that way round.
				const int32 Corner[4] = {
					Lattice.Index(X, Y),
					Lattice.Index(X + 1, Y),
					Lattice.Index(X + 1, Y + 1),
					Lattice.Index(X, Y + 1)
				};

				// Each side named by the lattice edge it runs along, so the quad
				// across it names the same one and they share the crossing.
				const int32 SideKey[4] = {
					Samples + Lattice.Index(X, Y),
					Samples * 2 + Lattice.Index(X + 1, Y),
					Samples + Lattice.Index(X, Y + 1),
					Samples * 2 + Lattice.Index(X, Y)
				};

				int32 PolyCount = 0;

				for (int32 Side = 0; Side < 4; ++Side)
				{
					const int32 A = Corner[Side];
					const int32 B = Corner[(Side + 1) % 4];

					const bool bInsideA = Lattice.Fill[A] > 0.f;
					const bool bInsideB = Lattice.Fill[B] > 0.f;

					if (bInsideA)
					{
						FCellVertex& Vertex = Poly[PolyCount++];

						Vertex.At = Lattice.At[A];
						Vertex.Top = Lattice.Top[A];
						Vertex.Bottom = Lattice.Bottom[A];

						// A corner lies on the two sides that meet at it.
						Vertex.OnSides = static_cast<uint8>(
							(1 << Side) | (1 << ((Side + 3) % 4)));
						Vertex.Key = A;
					}

					if (bInsideA != bInsideB)
					{
						Poly[PolyCount++] = Crossing(A, B, SideKey[Side],
							static_cast<uint8>(1 << Side));
					}
				}

				// Nothing, or a sliver too degenerate to have an area.
				if (PolyCount < 3)
				{
					continue;
				}

				// THE SADDLE RESOLVES ITSELF, which is the one case marching
				// squares is famous for being ambiguous about. Two opposite
				// corners inside and two outside can be read as one waist or as
				// two separate spurs; walking the boundary the way this loop does
				// always produces the connected reading, as a hexagon. Both are
				// legal, and picking the same one every time is what matters --
				// the crossings are shared with the neighbours regardless, so
				// there is no seam either way.
				for (int32 At = 0; At < PolyCount; ++At)
				{
					const FCellVertex& Vertex = Poly[At];

					TopIndex[At] = Reuse(TopVertexOf, Vertex.Key,
						Vertex.At.X, Vertex.At.Y, Vertex.Top);
					BottomIndex[At] = Reuse(BottomVertexOf, Vertex.Key,
						Vertex.At.X, Vertex.At.Y, Vertex.Bottom);
				}

				// A FAN IS ENOUGH: clipping a square by the contour leaves a convex
				// piece in every one of the sixteen cases, the hexagon above
				// included.
				for (int32 At = 1; At + 1 < PolyCount; ++At)
				{
					// Wound backwards from the outline's own direction, which is
					// what points a cap UP here.
					Mesh.AppendTriangle(TopIndex[0], TopIndex[At + 1], TopIndex[At]);

					// And the underside is the outline's winding untouched, so it
					// faces DOWN. A slab whose floor pointed up is invisible from
					// below and lets you see into it through the walls.
					Mesh.AppendTriangle(BottomIndex[0], BottomIndex[At], BottomIndex[At + 1]);
				}

				for (int32 At = 0; At < PolyCount; ++At)
				{
					const FCellVertex& A = Poly[At];
					const FCellVertex& B = Poly[(At + 1) % PolyCount];

					// A SEGMENT LYING ALONG ONE OF THE QUAD'S OWN SIDES IS NOT A
					// SILHOUETTE. The quad across that side shares both of its
					// endpoints and covers exactly the same stretch of it, so a
					// face here would be an interior wall buried in solid
					// material -- which is what the old box rule drew at every
					// cell boundary, and why a melted slab was full of them. Only
					// the segments the contour cut across the middle of a quad are
					// the edge of anything.
					if ((A.OnSides & B.OnSides) != 0)
					{
						continue;
					}

					// FRESH VERTICES, never the cap's, so the lip is a HARD edge --
					// see AppendWall, which does the same for the same reason.
					// Sharing them averages the cap's upward normal into the
					// wall's sideways one and rounds a cut in rock into a slump.
					const int32 TA = AppendSurfaceVertex(Mesh, Origin, A.At.X, A.At.Y, A.Top);
					const int32 TB = AppendSurfaceVertex(Mesh, Origin, B.At.X, B.At.Y, B.Top);
					const int32 BA = AppendSurfaceVertex(Mesh, Origin, A.At.X, A.At.Y, A.Bottom);
					const int32 BB = AppendSurfaceVertex(Mesh, Origin, B.At.X, B.At.Y, B.Bottom);

					Mesh.AppendTriangle(TA, BB, BA);
					Mesh.AppendTriangle(TA, TB, BB);
				}
			}
		}
	}
}

void BuildFieldMesh(UDynamicMeshComponent* Component, const FARPGSolidField& Field,
	const FVector2D& Origin, const FARPGSurfaceFacets& Facets)
{
	using namespace UE::Geometry;

	if (!Component)
	{
		return;
	}

	FDynamicMesh3 Mesh;
	Mesh.EnableVertexNormals(FVector3f::UnitZ());
	Mesh.EnableVertexUVs(FVector2f::Zero());

	if (!Field.IsValidField())
	{
		Component->SetMesh(MoveTemp(Mesh));
		Component->NotifyMeshUpdated();
		return;
	}

	const int32 Cells = Field.CountX * Field.CountY;

	TArray<float> Fill;
	TArray<double> BottomOf;
	TArray<double> TopOf;
	Fill.SetNumUninitialized(Cells);
	BottomOf.SetNumUninitialized(Cells);
	TopOf.SetNumUninitialized(Cells);

	for (int32 Y = 0; Y < Field.CountY; ++Y)
	{
		for (int32 X = 0; X < Field.CountX; ++X)
		{
			const int32 At = Field.Index(X, Y);

			// THE FIELD ALREADY KNOWS WHERE IT ENDS, to a fraction of a cell, and
			// says so as one number -- the outline it was cut with and the holes
			// melted through it, whichever is nearer. See SolidityAt.
			Fill[At] = Field.SolidityAt(X, Y);

			// AND WHAT AN EMPTY CELL'S HEIGHT OUGHT TO READ AS, which is not the
			// one it stores. See SurfaceTopAt -- this is the same pair of calls
			// TopAt interpolates, so the surface drawn here is the surface every
			// query answers from.
			BottomOf[At] = Field.SurfaceBottomAt(X, Y);
			TopOf[At] = Field.SurfaceTopAt(X, Y);
		}
	}

	BuildCellLayer(Mesh, Field, Origin, Fill, BottomOf, TopOf, Facets);

	FMeshNormals::QuickComputeVertexNormals(Mesh);

	Component->SetMesh(MoveTemp(Mesh));
	Component->NotifyMeshUpdated();
}

void BuildFilmMesh(UDynamicMeshComponent* Component, const FARPGSolidField& Field,
	const FVector2D& Origin, float MinimumFilm, const FARPGSurfaceFacets& Facets,
	TArrayView<const FARPGWallRun> Runs)
{
	using namespace UE::Geometry;

	if (!Component)
	{
		return;
	}

	FDynamicMesh3 Mesh;
	Mesh.EnableVertexNormals(FVector3f::UnitZ());
	Mesh.EnableVertexUVs(FVector2f::Zero());

	// EMPTY RATHER THAN EARLY, on the same reasoning as an eroded pool: the
	// component is still showing the last film it was given, and returning here
	// is how lava that has finished running off stays lying on the rock forever.
	if (!Field.IsValidField() || (!Field.HasWet() && Runs.Num() == 0))
	{
		Component->SetMesh(MoveTemp(Mesh));
		Component->NotifyMeshUpdated();
		return;
	}

	const int32 Cells = Field.CountX * Field.CountY;

	TArray<float> Fill;
	TArray<double> BottomOf;
	TArray<double> TopOf;
	Fill.SetNumUninitialized(Cells);
	BottomOf.SetNumUninitialized(Cells);
	TopOf.SetNumUninitialized(Cells);

	// A FILM TOO THIN TO SEE IS NOT WORTH A TRIANGLE, and the fluid already names
	// the depth below which it does not count as lying anywhere -- the same number
	// the flow solver stops moving at, so the drawn film and the simulated one
	// agree about where the edge of the wet is.
	const double Floor = FMath::Max(0.f, MinimumFilm);
	const float Band = FMath::Max(1.f, Field.CellSize);

	for (int32 Y = 0; Y < Field.CountY; ++Y)
	{
		for (int32 X = 0; X < Field.CountX; ++X)
		{
			const int32 At = Field.Index(X, Y);
			const double Depth = Field.Wet.IsValidIndex(At) ? Field.Wet[At] : 0.0;

			// WET, AND ON ROCK, taken as a minimum for the same reason the slab's
			// own two boundaries are -- lava stops either because it has run out or
			// because the rock under it has, and the nearer edge is the one you
			// see. Without the second half a film would mesh straight out over the
			// hole a fireball opened underneath it.
			Fill[At] = FMath::Min(Field.SolidityAt(X, Y),
				static_cast<float>(FMath::Clamp(Depth - Floor, -Band, Band)));

			// ON TOP OF THE ROCK, and on the rock's OWN reading of where its top
			// is -- the same call the slab's mesh is built from, so the lava
			// cannot end up hovering above the stone or sunk into it.
			//
			// A DRY CELL CONTRIBUTES NO DEPTH, which is what makes the film taper
			// to nothing at its rim rather than ending in a cliff: the surface is
			// interpolated between samples, and the sample just past the wet is
			// the rock itself.
			BottomOf[At] = Field.SurfaceTopAt(X, Y);
			TopOf[At] = BottomOf[At] + (Field.IsSolid(X, Y) ? Depth : 0.0);
		}
	}

	// THE SLAB'S OWN FACETS, not the fluid's, and that is not a mix-up. Relief
	// displaces the lattice by a hash of the corner index, so handing the film the
	// same settings makes it break over exactly the same bumps the rock does --
	// which is the only way a layer lying on a broken surface stays in contact
	// with it.
	BuildCellLayer(Mesh, Field, Origin, Fill, BottomOf, TopOf, Facets);

	// AND OVER THE EDGE. Everything above lies on cell TOPS, which is the whole of
	// what a heightfield can hold -- z = f(x, y) has no room for a vertical face,
	// so fluid reaching the rim of a slab leaves the field entirely.
	//
	// EACH RUN ONLY AS FAR AS IT HAS GOT. Drawing a full-height sheet the instant
	// a rim cell went wet was the first version of this, and it made every wall
	// read as though the lava had teleported to the bottom -- which is exactly
	// what the timed fall underneath it was doing. A run knows where its leading
	// edge is; the sheet stops there, and creeps as it does.
	const float Half = Field.CellSize * 0.5f;

	for (const FARPGWallRun& Run : Runs)
	{
		const FIntPoint Cell = Field.CellAt(Run.At);
		if (Cell.X < 0)
		{
			continue;
		}

		const int32 At = Field.Index(Cell.X, Cell.Y);

		// LIFTED WITH THE ROCK IT HANGS FROM. The face this sheet is drawn against
		// was displaced by the same hash of the same sample -- see BuildLattice --
		// so reading the stored height here would leave the top of a run of lava
		// floating a few centimetres off the lip it just went over.
		const double RockTop = Field.Top[At] * 0.1
			+ (Facets.Relief > 0.f
				? CornerNoise(Cell.X, Cell.Y, 3u) * Facets.Relief
				: 0.0);

		if (RockTop - Run.Front <= UE_DOUBLE_SMALL_NUMBER)
		{
			continue; // just gone over; nothing to see yet
		}

		const FVector2D Centre = Field.CentreOf(Cell.X, Cell.Y) - Origin;

		// The two corners of the side it went over, in the winding the faces use.
		const FVector2D Corners[4] = {
			Centre + FVector2D(-Half, -Half),
			Centre + FVector2D(Half, -Half),
			Centre + FVector2D(Half, Half),
			Centre + FVector2D(-Half, Half)
		};

		static const FIntPoint Steps[4] = { {0, -1}, {1, 0}, {0, 1}, {-1, 0} };

		int32 Side = INDEX_NONE;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			if (Steps[Index] == Run.Side)
			{
				Side = Index;
				break;
			}
		}

		if (Side == INDEX_NONE)
		{
			continue;
		}

		const int32 A = Side;
		const int32 B = (Side + 1) % 4;

		// OUTWARD BY ITS OWN THICKNESS, along the side's normal, so the sheet
		// stands off the face by as much as the film stands off the top. Any less
		// and it would be in the same plane as the rock and shimmer against it.
		const FVector2D Along = Corners[B] - Corners[A];
		const FVector2D Out = FVector2D(Along.Y, -Along.X).GetSafeNormal() * Run.Thickness;

		const FVector2D PA = Corners[A] + Out;
		const FVector2D PB = Corners[B] + Out;

		const int32 TA = AppendSurfaceVertex(Mesh, Origin, PA.X, PA.Y, RockTop);
		const int32 TB = AppendSurfaceVertex(Mesh, Origin, PB.X, PB.Y, RockTop);
		const int32 BA = AppendSurfaceVertex(Mesh, Origin, PA.X, PA.Y, Run.Front);
		const int32 BB = AppendSurfaceVertex(Mesh, Origin, PB.X, PB.Y, Run.Front);

		// Wound to face out of the slab -- see AppendWall. Nobody is ever behind a
		// sheet on the outside of a wall, so one face is all it needs and the
		// material need not be two-sided to show it.
		Mesh.AppendTriangle(TA, BB, BA);
		Mesh.AppendTriangle(TA, TB, BB);
	}

	FMeshNormals::QuickComputeVertexNormals(Mesh);

	Component->SetMesh(MoveTemp(Mesh));
	Component->NotifyMeshUpdated();
}

} // namespace ARPGFluidGeometry
