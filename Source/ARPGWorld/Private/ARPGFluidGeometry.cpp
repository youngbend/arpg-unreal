// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGFluidGeometry.h"
#include "CompGeom/ConstrainedDelaunay2.h"
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

	// Area scales with the SQUARE of a uniform scale, so this is one square root
	// rather than an iterative solve. Uniform rather than an inward offset
	// because an offset erodes thin necks away entirely and changes the shape --
	// wrong when the point is only that some of the fluid was consumed.
	const double Scale = FMath::Sqrt(TargetArea / Area);
	const FVector2D Centre = PolygonCentroid(Ring);

	TArray<FVector2D> Scaled;
	Scaled.Reserve(Ring.Num());

	for (const FVector2D& Point : Ring)
	{
		Scaled.Add(Centre + (Point - Centre) * Scale);
	}

	return Scaled;
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
	 */
	void AppendWall(UE::Geometry::FDynamicMesh3& Mesh, const TArray<FVector2D>& Ring,
		const FVector2D& Origin, double BottomZ, double TopZ)
	{
		const int32 Count = Ring.Num();

		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FVector2D& A = Ring[Index];
			const FVector2D& B = Ring[(Index + 1) % Count];

			// Fresh vertices per quad rather than shared with the cap, so the lip
			// of the slab is a HARD edge. Sharing them would average the cap's
			// upward normal into the wall's sideways one and round the whole thing
			// off, which on a 20cm puddle reads as a blob rather than as water
			// with an edge.
			const int32 TopA = AppendSurfaceVertex(Mesh, Origin, A.X, A.Y, TopZ);
			const int32 TopB = AppendSurfaceVertex(Mesh, Origin, B.X, B.Y, TopZ);
			const int32 BottomA = AppendSurfaceVertex(Mesh, Origin, A.X, A.Y, BottomZ);
			const int32 BottomB = AppendSurfaceVertex(Mesh, Origin, B.X, B.Y, BottomZ);

			Mesh.AppendTriangle(TopA, BottomA, BottomB);
			Mesh.AppendTriangle(TopA, BottomB, TopB);
		}
	}
}

void BuildSlabMesh(UDynamicMeshComponent* Component, const TArray<FVector2D>& Ring,
	const TArray<FVector2D>& Hole, const FVector2D& Origin, double BottomZ, double TopZ)
{
	using namespace UE::Geometry;

	if (!Component)
	{
		return;
	}

	FDynamicMesh3 Mesh;
	Mesh.EnableVertexNormals(FVector3f::UnitZ());
	Mesh.EnableVertexUVs(FVector2f::Zero());

	FGeneralPolygon2d Polygon;
	if (!ToGeneralPolygon(Ring, Polygon))
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
	if (Hole.Num() >= 3)
	{
		FPolygon2d HolePolygon;
		for (const FVector2D& Point : Hole)
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

	if (!Triangulator.Triangulate() || Triangulator.Triangles.Num() == 0)
	{
		Component->SetMesh(FDynamicMesh3());
		Component->NotifyMeshUpdated();
		return;
	}

	const int32 CapCount = Triangulator.Vertices.Num();
	const bool bHasDepth = TopZ - BottomZ > UE_DOUBLE_SMALL_NUMBER;

	for (const FVector2d& Vertex : Triangulator.Vertices)
	{
		AppendSurfaceVertex(Mesh, Origin, Vertex.X, Vertex.Y, TopZ);
	}

	if (bHasDepth)
	{
		for (const FVector2d& Vertex : Triangulator.Vertices)
		{
			AppendSurfaceVertex(Mesh, Origin, Vertex.X, Vertex.Y, BottomZ);
		}
	}

	for (const FIndex3i& Triangle : Triangulator.Triangles)
	{
		Mesh.AppendTriangle(Triangle.A, Triangle.B, Triangle.C);

		if (bHasDepth)
		{
			// Reversed, so the underside faces DOWN. A one-sided slab whose floor
			// pointed up is invisible from below and lets you see into it through
			// the walls.
			Mesh.AppendTriangle(Triangle.A + CapCount, Triangle.C + CapCount,
				Triangle.B + CapCount);
		}
	}

	if (bHasDepth)
	{
		AppendWall(Mesh, FromPolygon(Polygon.GetOuter()), Origin, BottomZ, TopZ);

		if (WoundHole.Num() >= 3)
		{
			AppendWall(Mesh, WoundHole, Origin, BottomZ, TopZ);
		}
	}

	FMeshNormals::QuickComputeVertexNormals(Mesh);

	Component->SetMesh(MoveTemp(Mesh));
	Component->NotifyMeshUpdated();
}

void BuildFieldMesh(UDynamicMeshComponent* Component, const FARPGIceField& Field,
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

	const float Half = Field.CellSize * 0.5f;

	// A CELL AT A TIME, and that is the point. Nothing here triangulates an
	// outline, so nothing here can grow: the worst case is a fixed handful of
	// triangles per cell however many spells have landed on the floe. The Godot
	// pathology this replaced was unbounded by construction.
	for (int32 Y = 0; Y < Field.CountY; ++Y)
	{
		for (int32 X = 0; X < Field.CountX; ++X)
		{
			if (!Field.IsIced(X, Y))
			{
				continue;
			}

			const int32 At = Field.Index(X, Y);
			const FVector2D Centre = Field.CentreOf(X, Y) - Origin;

			const double TopZ = Field.Top[At] * 0.1;
			const double BottomZ = Field.Bottom[At] * 0.1;

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

			Mesh.AppendTriangle(TopVerts[0], TopVerts[1], TopVerts[2]);
			Mesh.AppendTriangle(TopVerts[0], TopVerts[2], TopVerts[3]);

			Mesh.AppendTriangle(BottomVerts[0], BottomVerts[2], BottomVerts[1]);
			Mesh.AppendTriangle(BottomVerts[0], BottomVerts[3], BottomVerts[2]);

			// A WALL ONLY WHERE THE ICE STOPS. Between two iced cells there is
			// nothing to see and a face there would be interior geometry the
			// collision cook has to chew through for no reason. The neighbours
			// that are missing are the silhouette -- and the rim of a hole is the
			// same test, which is how a hole gets its inside face without being a
			// thing anyone tracked.
			static const FIntPoint Steps[4] = { {0, -1}, {1, 0}, {0, 1}, {-1, 0} };

			for (int32 Side = 0; Side < 4; ++Side)
			{
				if (Field.IsIced(X + Steps[Side].X, Y + Steps[Side].Y))
				{
					continue;
				}

				const int32 A = Side;
				const int32 B = (Side + 1) % 4;

				Mesh.AppendTriangle(TopVerts[A], BottomVerts[A], BottomVerts[B]);
				Mesh.AppendTriangle(TopVerts[A], BottomVerts[B], TopVerts[B]);
			}
		}
	}

	FMeshNormals::QuickComputeVertexNormals(Mesh);

	Component->SetMesh(MoveTemp(Mesh));
	Component->NotifyMeshUpdated();
}

} // namespace ARPGFluidGeometry
