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
	 * One layer of cells drawn as a box each: a cap top, a cap bottom, and a face
	 * on any side where this cell's span is not covered by its neighbour's.
	 *
	 * SHARED BY THE ROCK AND THE LAVA LYING ON IT, which are the same shape
	 * problem asked of two different pairs of heights -- the solid runs Bottom to
	 * Top, the film runs Top to Top plus however deep it lies. Written once
	 * because the side-face rule is the part that is easy to get wrong, and
	 * getting it wrong in one of two copies is how a melted slab came to be open
	 * at every terrace a spell had cut into it.
	 *
	 * Spans are LOCAL Z in centimetres, indexed as the field is. A cell whose
	 * Present is false is a gap, and its neighbours draw a full-height face at it.
	 */
	void BuildCellLayer(UE::Geometry::FDynamicMesh3& Mesh, const FARPGSolidField& Field,
		const FVector2D& Origin, const TArray<bool>& Present,
		const TArray<double>& BottomOf, const TArray<double>& TopOf)
	{
		const float Half = Field.CellSize * 0.5f;

		// A CELL AT A TIME, and that is the point. Nothing here triangulates an
		// outline, so nothing here can grow: the worst case is a fixed handful of
		// triangles per cell however many spells have landed on the floe. The Godot
		// pathology this replaced was unbounded by construction.
		for (int32 Y = 0; Y < Field.CountY; ++Y)
		{
			for (int32 X = 0; X < Field.CountX; ++X)
			{
				const int32 At = Field.Index(X, Y);

				if (!Present[At] || TopOf[At] - BottomOf[At] <= UE_DOUBLE_SMALL_NUMBER)
				{
					continue;
				}

				const FVector2D Centre = Field.CentreOf(X, Y) - Origin;
				const double TopZ = TopOf[At];
				const double BottomZ = BottomOf[At];

				const FVector2D Corners[4] = {
					Centre + FVector2D(-Half, -Half),
					Centre + FVector2D(Half, -Half),
					Centre + FVector2D(Half, Half),
					Centre + FVector2D(-Half, Half)
				};

				int32 TopVerts[4];
				int32 BottomVerts[4];

				for (int32 Corner = 0; Corner < 4; ++Corner)
				{
					TopVerts[Corner] = AppendSurfaceVertex(Mesh, Origin,
						Corners[Corner].X, Corners[Corner].Y, TopZ);
					BottomVerts[Corner] = AppendSurfaceVertex(Mesh, Origin,
						Corners[Corner].X, Corners[Corner].Y, BottomZ);
				}

				// CLOCKWISE IN XY, which is what points a face UP here. Unreal is
				// left-handed and FDynamicMesh3 normals are (C - A) x (B - A) --
				// see AppendWall. The counter-clockwise winding that reads as "up"
				// everywhere else turns the cell inside out.
				Mesh.AppendTriangle(TopVerts[0], TopVerts[2], TopVerts[1]);
				Mesh.AppendTriangle(TopVerts[0], TopVerts[3], TopVerts[2]);

				Mesh.AppendTriangle(BottomVerts[0], BottomVerts[1], BottomVerts[2]);
				Mesh.AppendTriangle(BottomVerts[0], BottomVerts[2], BottomVerts[3]);

				// A FACE WHEREVER THIS CELL STANDS PROUD OF ITS NEIGHBOUR, which is
				// not the same question as whether the neighbour is there at all.
				//
				// THIS USED TO SKIP EVERY PRESENT NEIGHBOUR, on the reasoning that
				// between two iced cells there is nothing to see. True only while
				// the layer is flat. The moment a spell melts a bowl into it the
				// cells have DIFFERENT tops, and the step between them is a face
				// nobody drew -- so the terrace it cut was open at the side and you
				// could see, and walk, straight into the middle of the rock.
				//
				// The exposed band is whatever part of this cell's span the
				// neighbour's own span does not cover. An absent neighbour covers
				// nothing, which is the old behaviour falling out of the general
				// rule rather than sitting beside it as a case.
				static const FIntPoint Steps[4] = { {0, -1}, {1, 0}, {0, 1}, {-1, 0} };

				// FRESH VERTICES PER BAND rather than the cap's, so the lip of
				// every terrace is a HARD edge -- see AppendWall, which does the
				// same for the same reason. Sharing them averaged the cap's upward
				// normal into the wall's sideways one and rounded a cut in rock
				// into a slump.
				auto AppendBand = [&](int32 CornerA, int32 CornerB, double FromZ, double ToZ)
				{
					if (ToZ - FromZ <= UE_DOUBLE_SMALL_NUMBER)
					{
						return;
					}

					const FVector2D& PA = Corners[CornerA];
					const FVector2D& PB = Corners[CornerB];

					const int32 TA = AppendSurfaceVertex(Mesh, Origin, PA.X, PA.Y, ToZ);
					const int32 TB = AppendSurfaceVertex(Mesh, Origin, PB.X, PB.Y, ToZ);
					const int32 BA = AppendSurfaceVertex(Mesh, Origin, PA.X, PA.Y, FromZ);
					const int32 BB = AppendSurfaceVertex(Mesh, Origin, PB.X, PB.Y, FromZ);

					Mesh.AppendTriangle(TA, BB, BA);
					Mesh.AppendTriangle(TA, TB, BB);
				};

				for (int32 Side = 0; Side < 4; ++Side)
				{
					const int32 NeighbourX = X + Steps[Side].X;
					const int32 NeighbourY = Y + Steps[Side].Y;

					const bool bInside = NeighbourX >= 0 && NeighbourX < Field.CountX
						&& NeighbourY >= 0 && NeighbourY < Field.CountY;

					const int32 NeighbourAt = bInside ? Field.Index(NeighbourX, NeighbourY) : 0;
					const bool bNeighbour = bInside && Present[NeighbourAt];

					// COLLAPSED ONTO THIS CELL'S FLOOR when there is no neighbour,
					// so the two bands below come out as the one full-height face
					// the silhouette wants without a branch of their own.
					const double NeighbourTop = bNeighbour ? TopOf[NeighbourAt] : BottomZ;
					const double NeighbourBottom = bNeighbour ? BottomOf[NeighbourAt] : BottomZ;

					const int32 A = Side;
					const int32 B = (Side + 1) % 4;

					// Standing above the neighbour: the riser of a terrace.
					AppendBand(A, B, FMath::Max(BottomZ, NeighbourTop), TopZ);

					// And hanging below it, which is the same step seen from a cell
					// melted from underneath rather than from on top.
					AppendBand(A, B, BottomZ, FMath::Min(TopZ, NeighbourBottom));
				}
			}
		}
	}
}

void BuildFieldMesh(UDynamicMeshComponent* Component, const FARPGSolidField& Field,
	const FVector2D& Origin)
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

	TArray<bool> Present;
	TArray<double> BottomOf;
	TArray<double> TopOf;
	Present.SetNumUninitialized(Cells);
	BottomOf.SetNumUninitialized(Cells);
	TopOf.SetNumUninitialized(Cells);

	for (int32 Y = 0; Y < Field.CountY; ++Y)
	{
		for (int32 X = 0; X < Field.CountX; ++X)
		{
			const int32 At = Field.Index(X, Y);
			Present[At] = Field.IsSolid(X, Y);
			BottomOf[At] = Field.Bottom[At] * 0.1;
			TopOf[At] = Field.Top[At] * 0.1;
		}
	}

	BuildCellLayer(Mesh, Field, Origin, Present, BottomOf, TopOf);

	FMeshNormals::QuickComputeVertexNormals(Mesh);

	Component->SetMesh(MoveTemp(Mesh));
	Component->NotifyMeshUpdated();
}

void BuildFilmMesh(UDynamicMeshComponent* Component, const FARPGSolidField& Field,
	const FVector2D& Origin, float MinimumFilm, TArrayView<const FARPGWallRun> Runs)
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

	TArray<bool> Present;
	TArray<double> BottomOf;
	TArray<double> TopOf;
	Present.SetNumUninitialized(Cells);
	BottomOf.SetNumUninitialized(Cells);
	TopOf.SetNumUninitialized(Cells);

	// A FILM TOO THIN TO SEE IS NOT WORTH A TRIANGLE, and the fluid already names
	// the depth below which it does not count as lying anywhere -- the same number
	// the flow solver stops moving at, so the drawn film and the simulated one
	// agree about where the edge of the wet is.
	const double Floor = FMath::Max(0.f, MinimumFilm);

	for (int32 Y = 0; Y < Field.CountY; ++Y)
	{
		for (int32 X = 0; X < Field.CountX; ++X)
		{
			const int32 At = Field.Index(X, Y);
			const double Depth = Field.Wet.IsValidIndex(At) ? Field.Wet[At] : 0.0;

			Present[At] = Field.IsSolid(X, Y) && Depth > Floor;

			// ON TOP OF THE ROCK, so the film sits in whatever bowl the rock was
			// cut into and its own top is the rock's plus how deep it lies. Both
			// read the same heightfield, so lava cannot drift off the surface it
			// is lying on.
			BottomOf[At] = Field.Top[At] * 0.1;
			TopOf[At] = BottomOf[At] + Depth;
		}
	}

	BuildCellLayer(Mesh, Field, Origin, Present, BottomOf, TopOf);

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
		const double RockTop = Field.Top[At] * 0.1;

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
