// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGSolidField.h"
#include "ARPGFluidGeometry.h"

namespace
{
	/** Millimetres to centimetres and back -- see the field's storage note. */
	constexpr float ToCm = 0.1f;
	constexpr float ToMm = 10.f;

	/** Below this a cell is bare fluid rather than a very thin skin, in cm. */
	constexpr float MinimumLayer = 1.f;

	int16 Clamped(float Millimetres)
	{
		return static_cast<int16>(FMath::Clamp(Millimetres,
			static_cast<float>(MIN_int16), static_cast<float>(MAX_int16)));
	}
}

FIntPoint FARPGSolidField::CellAt(const FVector2D& World) const
{
	if (CellSize <= 0.f)
	{
		return FIntPoint(-1, -1);
	}

	const int32 X = FMath::FloorToInt32((World.X - Origin.X) / CellSize + 0.5f);
	const int32 Y = FMath::FloorToInt32((World.Y - Origin.Y) / CellSize + 0.5f);

	return (X >= 0 && Y >= 0 && X < CountX && Y < CountY) ? FIntPoint(X, Y) : FIntPoint(-1, -1);
}

FVector2D FARPGSolidField::CentreOf(int32 X, int32 Y) const
{
	return Origin + FVector2D(X * CellSize, Y * CellSize);
}

bool FARPGSolidField::IsSolid(int32 X, int32 Y) const
{
	return ThicknessAt(X, Y) >= MinimumLayer;
}

float FARPGSolidField::ThicknessAt(int32 X, int32 Y) const
{
	if (!IsValidField() || X < 0 || Y < 0 || X >= CountX || Y >= CountY)
	{
		return 0.f;
	}

	const int32 At = Index(X, Y);
	return FMath::Max(0.f, (Top[At] - Bottom[At]) * ToCm);
}

float FARPGSolidField::InsetAt(int32 X, int32 Y) const
{
	const bool bOnGrid = X >= 0 && Y >= 0 && X < CountX && Y < CountY;

	// ONE CELL EITHER SIDE is the whole range the plane carries -- see Edge -- so
	// it is also the range everything off the end of it saturates to.
	const float Band = FMath::Max(1.f, CellSize);

	if (!bOnGrid)
	{
		return -Band;
	}

	// NO OUTLINE MEANS NOTHING TO BE OUTSIDE OF. A field that never had the plane
	// -- one adopted from an older save, or a bare struct a test built by hand --
	// falls back to letting thickness alone decide, which is exactly the cell rule
	// this replaced.
	if (Edge.Num() != Top.Num())
	{
		return Band;
	}

	return FMath::Clamp(-Edge[Index(X, Y)] * ToCm, -Band, Band);
}

float FARPGSolidField::SolidityAt(int32 X, int32 Y) const
{
	const float Band = FMath::Max(1.f, CellSize);

	// AGAINST THE SAME FLOOR IsSolid USES, so the drawn edge and the walkable one
	// are answers to one question. Clamped into the band alongside the inset for
	// the same reason it is: a thickness of metres would otherwise swamp a
	// distance of centimetres in the minimum below and the outline would stop
	// being able to cut anything.
	const float Material = FMath::Clamp(ThicknessAt(X, Y) - MinimumLayer, -Band, Band);

	return FMath::Min(InsetAt(X, Y), Material);
}

float FARPGSolidField::SolidityAtWorld(const FVector2D& World) const
{
	if (!IsValidField() || CellSize <= 0.f)
	{
		return -1.f;
	}

	// THE SAME INTERPOLATION THE MESH IS CUT WITH. Cell values sit at cell
	// CENTRES, so the four surrounding a point are the four whose centres box it
	// in -- which is the floor of the point in cell coordinates, not the cell it
	// happens to be inside.
	const FVector2D Cell = (World - Origin) / CellSize;

	const int32 X = FMath::FloorToInt32(Cell.X);
	const int32 Y = FMath::FloorToInt32(Cell.Y);

	const float TX = static_cast<float>(Cell.X) - X;
	const float TY = static_cast<float>(Cell.Y) - Y;

	const float Low = FMath::Lerp(SolidityAt(X, Y), SolidityAt(X + 1, Y), TX);
	const float High = FMath::Lerp(SolidityAt(X, Y + 1), SolidityAt(X + 1, Y + 1), TX);

	return FMath::Lerp(Low, High, TY);
}

bool FARPGSolidField::IsSolidAt(const FVector2D& World) const
{
	return SolidityAtWorld(World) > 0.f;
}

float FARPGSolidField::BorrowedAt(const TArray<int16>& Plane, int32 X, int32 Y) const
{
	if (!IsValidField() || Plane.Num() != Top.Num()
		|| X < 0 || Y < 0 || X >= CountX || Y >= CountY)
	{
		return 0.f;
	}

	if (IsSolid(X, Y))
	{
		return Plane[Index(X, Y)] * ToCm;
	}

	// THE FOUR SHARING A SIDE, and only the ones with material in them. See
	// SurfaceTopAt for why an empty cell's own stored height is the one number
	// that must not be read here.
	static const FIntPoint Neighbours[4] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };

	float Sum = 0.f;
	int32 Count = 0;

	for (const FIntPoint& Step : Neighbours)
	{
		const int32 NX = X + Step.X;
		const int32 NY = Y + Step.Y;

		if (NX < 0 || NY < 0 || NX >= CountX || NY >= CountY || !IsSolid(NX, NY))
		{
			continue;
		}

		Sum += Plane[Index(NX, NY)] * ToCm;
		++Count;
	}

	// Nothing anywhere near it has material either, so there is no height to
	// borrow and its own is as good an answer as exists.
	return Count > 0 ? Sum / Count : Plane[Index(X, Y)] * ToCm;
}

float FARPGSolidField::SurfaceTopAt(int32 X, int32 Y) const
{
	return BorrowedAt(Top, X, Y);
}

float FARPGSolidField::SurfaceBottomAt(int32 X, int32 Y) const
{
	return BorrowedAt(Bottom, X, Y);
}

float FARPGSolidField::TopAt(const FVector2D& World) const
{
	if (!IsValidField() || CellSize <= 0.f)
	{
		return 0.f;
	}

	// OFF THE SLAB IS STILL ZERO. Everything that asks this pairs it with a
	// containment test, and answering with a neighbour's height for a point
	// nowhere near the slab would give a caller a surface to stand on that is not
	// there.
	if (CellAt(World).X < 0)
	{
		return 0.f;
	}

	// THE SAME FOUR SAMPLES, IN THE SAME ORDER, AS THE MESHER'S -- see
	// SolidityAtWorld, which interpolates the shape exactly as this interpolates
	// the height. Cell values sit at cell CENTRES, so the four surrounding a point
	// are the four whose centres box it in.
	const FVector2D Cell = (World - Origin) / CellSize;

	const int32 X = FMath::FloorToInt32(Cell.X);
	const int32 Y = FMath::FloorToInt32(Cell.Y);

	const float TX = static_cast<float>(Cell.X) - X;
	const float TY = static_cast<float>(Cell.Y) - Y;

	const float Low = FMath::Lerp(SurfaceTopAt(X, Y), SurfaceTopAt(X + 1, Y), TX);
	const float High = FMath::Lerp(SurfaceTopAt(X, Y + 1), SurfaceTopAt(X + 1, Y + 1), TX);

	return FMath::Lerp(Low, High, TY);
}

void FARPGSolidField::Refresh()
{
	const double CellArea = static_cast<double>(CellSize) * CellSize;

	FVector2D Sum = FVector2D::ZeroVector;
	CachedCells = 0;
	CachedVolume = 0.0;

	// ONE SWEEP for all four answers, and only when something has written. The
	// buoyancy tick asks for every one of them every frame; sweeping per question
	// per frame is what made a floe cost its whole grid ten times over per tick.
	for (int32 Y = 0; Y < CountY; ++Y)
	{
		for (int32 X = 0; X < CountX; ++X)
		{
			if (!IsSolid(X, Y))
			{
				continue;
			}

			++CachedCells;
			CachedVolume += ThicknessAt(X, Y) * CellArea;
			Sum += CentreOf(X, Y);
		}
	}

	CachedArea = CachedCells * CellArea;
	CachedCentroid = CachedCells > 0 ? Sum / CachedCells : Origin;

	RefreshWet();
}

void FARPGSolidField::RefreshWet()
{
	CachedWet = 0.0;

	// A REBUILD DROPS THE FILM. The array is sized to the cells and a field that
	// has just been rebuilt from a ring has no relationship to whatever was on the
	// old one, so keeping it would put water at arbitrary places on a new shape.
	if (Wet.Num() != Top.Num())
	{
		Wet.Reset();
		return;
	}

	const double CellArea = static_cast<double>(CellSize) * CellSize;

	for (int32 Y = 0; Y < CountY; ++Y)
	{
		for (int32 X = 0; X < CountX; ++X)
		{
			const int32 At = Index(X, Y);

			// WATER ON A CELL THAT JUST MELTED THROUGH HAS NOWHERE TO BE. FlowStep
			// skips cells that are not solid, so leaving it would strand a volume
			// that keeps CachedWet above zero and keeps the slab ticking for the
			// rest of the level. Dropped rather than shed: it is the film that was
			// sitting on the bit of slab a fireball just removed, and the same
			// fireball is producing far more meltwater through the front door.
			if (!IsSolid(X, Y))
			{
				Wet[At] = 0.f;
				continue;
			}

			CachedWet += static_cast<double>(Wet[At]) * CellArea;
		}
	}
}

// ---------------------------------------------------------------------------
// The film running over it
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Getting it across the wire
// ---------------------------------------------------------------------------

namespace
{
	/**
	 * Run-length codec for one plane of the grid.
	 *
	 * A COUNT AND A VALUE, with the count as a byte and a 0 escape for anything
	 * longer -- so a uniform 2500-cell plane is ten runs of 255 rather than one
	 * run needing a wider count on every run in every message. Runs are almost
	 * always either very long (untouched slab) or very short (the rim of a bowl),
	 * and a byte serves both without a variable-width integer.
	 */
	void WriteRuns(FArchive& Ar, const TArray<int16>& Values)
	{
		int32 At = 0;

		while (At < Values.Num())
		{
			const int16 Value = Values[At];
			int32 Run = 1;

			while (At + Run < Values.Num() && Values[At + Run] == Value && Run < 255)
			{
				++Run;
			}

			uint8 Count = static_cast<uint8>(Run);
			int16 Written = Value;

			Ar << Count;
			Ar << Written;

			At += Run;
		}
	}

	void ReadRuns(FArchive& Ar, TArray<int16>& Values, int32 Cells)
	{
		Values.SetNumUninitialized(Cells);

		int32 At = 0;

		while (At < Cells)
		{
			uint8 Count = 0;
			int16 Value = 0;

			Ar << Count;
			Ar << Value;

			// A ZERO COUNT WOULD NOT ADVANCE, and a stream that says so is either
			// corrupt or from a different build. Filling the rest and leaving is
			// better than spinning here forever.
			if (Count == 0)
			{
				for (; At < Cells; ++At)
				{
					Values[At] = Value;
				}
				return;
			}

			const int32 Run = FMath::Min(static_cast<int32>(Count), Cells - At);

			for (int32 Step = 0; Step < Run; ++Step)
			{
				Values[At + Step] = Value;
			}

			At += Run;
		}
	}
}

bool FARPGSolidField::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	bOutSuccess = true;

	Ar << Origin;
	Ar << CellSize;
	Ar << CountX;
	Ar << CountY;

	const int32 Cells = CountX * CountY;

	if (Cells <= 0)
	{
		if (Ar.IsLoading())
		{
			Top.Reset();
			Bottom.Reset();
			Edge.Reset();
		}
		return true;
	}

	if (Ar.IsSaving())
	{
		// A grid whose planes do not match its own dimensions is a bug elsewhere,
		// but sending a short one would desynchronise the reader's run count and
		// corrupt everything after it in the bunch.
		if (Top.Num() != Cells)
		{
			Top.SetNumZeroed(Cells);
		}
		if (Bottom.Num() != Cells)
		{
			Bottom.SetNumZeroed(Cells);
		}

		// A THIRD PLANE, AND THE CHEAPEST OF THE THREE. Edge is clamped to one
		// cell either side of the outline, so it is one run for the whole interior,
		// one for the whole exterior, and detail only along the band between --
		// a perimeter's worth of cells against an area's. Sending the shape costs
		// less than sending the heights it holds.
		if (Edge.Num() != Cells)
		{
			// A slab with no outline plane is meshed by the cell rule instead, so
			// a uniform interior is the reading that changes nothing.
			Edge.Init(static_cast<int16>(-FMath::Max(1.f, CellSize) * 10.f), Cells);
		}

		WriteRuns(Ar, Top);
		WriteRuns(Ar, Bottom);
		WriteRuns(Ar, Edge);

		return true;
	}

	ReadRuns(Ar, Top, Cells);
	ReadRuns(Ar, Bottom, Cells);
	ReadRuns(Ar, Edge, Cells);

	// The totals are not sent -- they are pure functions of the cells -- so a
	// receiver puts them back itself. Cheaper than the bytes it saves, and it
	// cannot disagree with what arrived.
	Refresh();

	return true;
}

float FARPGSolidField::WetAt(const FVector2D& World) const
{
	if (Wet.Num() != Top.Num())
	{
		return 0.f;
	}

	const FIntPoint Cell = CellAt(World);
	return Cell.X >= 0 ? Wet[Index(Cell.X, Cell.Y)] : 0.f;
}

double FARPGSolidField::WetVolume() const
{
	return CachedWet;
}

double FARPGSolidField::Pour(const FVector2D& At, float Radius, double Volume)
{
	if (Volume <= 0.0 || !IsValidField())
	{
		return Volume;
	}

	// The film array is grown lazily. A slab that never melts never pays for it,
	// which is most of them -- an earth wall exists to be stood behind.
	if (Wet.Num() != Top.Num())
	{
		Wet.SetNumZeroed(Top.Num());
		CachedWet = 0.0;
	}

	// EVERY CELL THE DISC TOUCHES, solid or not. The ones that are not solid are
	// counted and then given nothing, so fluid poured over a hole falls through
	// in proportion rather than piling onto the rim -- which is what a bowl melted
	// clean through should do.
	const float Reach = FMath::Max(Radius, CellSize);
	const double ReachSq = static_cast<double>(Reach) * Reach;

	const FIntPoint Low = CellAt(At - FVector2D(Reach, Reach));
	const FIntPoint High = CellAt(At + FVector2D(Reach, Reach));

	const int32 MinX = FMath::Max(0, Low.X >= 0 ? Low.X : 0);
	const int32 MinY = FMath::Max(0, Low.Y >= 0 ? Low.Y : 0);
	const int32 MaxX = FMath::Min(CountX - 1, High.X >= 0 ? High.X : CountX - 1);
	const int32 MaxY = FMath::Min(CountY - 1, High.Y >= 0 ? High.Y : CountY - 1);

	int32 Covered = 0;
	int32 Landed = 0;

	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			if (FVector2D::DistSquared(CentreOf(X, Y), At) > ReachSq)
			{
				continue;
			}

			++Covered;
			Landed += IsSolid(X, Y) ? 1 : 0;
		}
	}

	// Nothing under the pour at all -- the disc is off the grid, or entirely over
	// a hole. All of it falls.
	if (Covered == 0 || Landed == 0)
	{
		return Volume;
	}

	const double CellArea = static_cast<double>(CellSize) * CellSize;
	const double Share = Volume / Covered;
	const float Depth = static_cast<float>(Share / CellArea);

	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			if (!IsSolid(X, Y) || FVector2D::DistSquared(CentreOf(X, Y), At) > ReachSq)
			{
				continue;
			}

			Wet[Index(X, Y)] += Depth;
			CachedWet += Share;
		}
	}

	// What fell through the holes, for the caller to put on the ground.
	return Share * (Covered - Landed);
}

double FARPGSolidField::FlowStep(float DeltaTime, float Rate, float YieldSlope,
	float MinimumFilm, FVector2D& OutShedAt)
{
	OutShedAt = CachedCentroid;

	if (!HasWet() || Wet.Num() != Top.Num() || DeltaTime <= 0.f)
	{
		return 0.0;
	}

	const double CellArea = static_cast<double>(CellSize) * CellSize;
	const float Step = FMath::Clamp(Rate * DeltaTime, 0.f, 1.f);

	// DOUBLE BUFFERED. A single sweep in raster order would let a cell read its
	// eastern neighbour's depth from this step and its western neighbour's from
	// the last, so water would run downhill faster in one direction than the
	// other -- a bias you can see, on a symmetrical dome, as a film that drifts.
	TArray<float> Next = Wet;

	double Shed = 0.0;
	FVector2D ShedMoment = FVector2D::ZeroVector;

	// Four-neighbour rather than eight. A diagonal step is 1.41 cells long and
	// weighting for that is the sort of correction whose absence nobody sees on a
	// film, while the extra four lookups per cell are paid on every one of them.
	const FIntPoint Steps[4] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };

	for (int32 Y = 0; Y < CountY; ++Y)
	{
		for (int32 X = 0; X < CountX; ++X)
		{
			const int32 Here = Index(X, Y);
			const float Depth = Wet[Here];

			if (Depth <= 0.f || !IsSolid(X, Y))
			{
				continue;
			}

			// Slab-local cm. Top is millimetres, the film is centimetres, and this
			// is the one place the two are added -- get it wrong and a 30cm slab
			// looks like a 3m cliff to the water on it.
			const float Surface = Top[Here] * 0.1f + Depth;

			float Drops[4] = { 0.f, 0.f, 0.f, 0.f };
			float TotalDrop = 0.f;

			for (int32 Side = 0; Side < 4; ++Side)
			{
				const int32 NX = X + Steps[Side].X;
				const int32 NY = Y + Steps[Side].Y;

				const bool bOffGrid = NX < 0 || NY < 0 || NX >= CountX || NY >= CountY;

				// OFF THE EDGE OR INTO A HOLE IS A CLIFF, not a neighbour. There is
				// nothing over there to hold water at any height, so the whole of
				// this cell's own surface is the drop -- which is what makes a film
				// pour off the rim of a slab instead of pooling against it.
				const float Drop = (bOffGrid || !IsSolid(NX, NY))
					? Surface
					: Surface - (Top[Index(NX, NY)] * 0.1f + Wet[Index(NX, NY)]);

				// THE YIELD SLOPE, which is the half of viscosity a rate cannot
				// express. A rate makes a fluid arrive later; this makes it STOP.
				// Lava on a gradient water would sheet off simply sits there, and
				// because it needs more head before it will go anywhere it also
				// stands thicker -- the piling-up nobody had to write.
				//
				// An edge is exempt. Nothing is holding the fluid back over a
				// cliff, however thick it is, and a viscous flow that reached the
				// rim of a slab and refused to fall off it would be a bug rather
				// than a behaviour.
				if (Drop > YieldSlope || (Drop > 0.f && (bOffGrid || !IsSolid(NX, NY))))
				{
					Drops[Side] = Drop;
					TotalDrop += Drop;
				}
			}

			if (TotalDrop <= 0.f)
			{
				continue; // sitting in a dish with nowhere lower to go
			}

			// HALF THE HEAD, not all of it. Two cells trading across a step want to
			// settle level; moving the whole difference overshoots and they swap
			// heights forever, which on a flat slab reads as a shimmer.
			const float Moving = FMath::Min(Depth, TotalDrop * 0.5f) * Step;
			if (Moving <= 0.f)
			{
				continue;
			}

			Next[Here] -= Moving;

			for (int32 Side = 0; Side < 4; ++Side)
			{
				if (Drops[Side] <= 0.f)
				{
					continue;
				}

				const float Portion = Moving * (Drops[Side] / TotalDrop);

				const int32 NX = X + Steps[Side].X;
				const int32 NY = Y + Steps[Side].Y;
				const bool bOffGrid = NX < 0 || NY < 0 || NX >= CountX || NY >= CountY;

				if (bOffGrid || !IsSolid(NX, NY))
				{
					// GONE FROM THE SLAB. Weighted by where it left, so a tower
					// melted on one side sheds down that side and the puddle forms
					// there rather than under the middle.
					const double Volume = static_cast<double>(Portion) * CellArea;

					Shed += Volume;
					ShedMoment += CentreOf(X, Y) * Volume;
					continue;
				}

				Next[Index(NX, NY)] += Portion;
			}
		}
	}

	// A FLOOR, or the film never finishes. Each step moves a fraction of what is
	// left, so depth approaches zero and never arrives -- and a slab with a
	// millionth of a millimetre on it would tick, rebuild and re-cook forever.
	// What is swept up this way is dried in place rather than shed: it is a damp
	// patch, not a drip, and inventing a puddle out of it would be worse.
	double Remaining = 0.0;

	for (float& Depth : Next)
	{
		if (Depth < MinimumFilm)
		{
			Depth = 0.f;
			continue;
		}

		Remaining += static_cast<double>(Depth) * CellArea;
	}

	Wet = MoveTemp(Next);
	CachedWet = Remaining;

	if (Shed > 0.0)
	{
		OutShedAt = ShedMoment / Shed;
	}

	return Shed;
}

double FARPGSolidField::SupportDistance(const FVector2D& From, const FVector2D& Direction) const
{
	double Furthest = 0.0;

	for (int32 Y = 0; Y < CountY; ++Y)
	{
		for (int32 X = 0; X < CountX; ++X)
		{
			if (IsSolid(X, Y))
			{
				Furthest = FMath::Max(Furthest,
					FVector2D::DotProduct(CentreOf(X, Y) - From, Direction));
			}
		}
	}

	// Half a cell, because the support is measured to cell CENTRES and the material in
	// the furthest one reaches to its edge.
	return Furthest + CellSize * 0.5;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

void FARPGSolidField::BuildFrom(const TArray<FVector2D>& Ring, float InCellSize, float Thickness)
{
	Top.Reset();
	Bottom.Reset();
	Edge.Reset();
	CountX = 0;
	CountY = 0;

	if (Ring.Num() < 3 || InCellSize <= 0.f || Thickness <= 0.f)
	{
		return;
	}

	CellSize = InCellSize;

	// One cell of margin, so the slab never sits flush against the edge of its own
	// grid -- refreezing can add to a floe and would have nowhere to put it.
	const FBox2D Bounds = ARPGFluidGeometry::PolygonBounds(Ring).ExpandBy(CellSize);

	Origin = Bounds.Min;
	CountX = FMath::CeilToInt32(Bounds.GetSize().X / CellSize) + 1;
	CountY = FMath::CeilToInt32(Bounds.GetSize().Y / CellSize) + 1;

	Top.SetNumZeroed(CountX * CountY);
	Bottom.SetNumZeroed(CountX * CountY);
	Edge.SetNumZeroed(CountX * CountY);

	// ZERO IS THE UNDERSIDE the slab started at, and everything vertical is
	// relative to it -- which is what lets the whole field ride up and down on the
	// buoyancy without a single cell being rewritten.
	const int16 Surface = Clamped(Thickness * ToMm);

	// See Edge: further than a cell away says nothing the mesher can use, and
	// clamping is what leaves the interior and the exterior as one run each.
	const float Band = CellSize * ToMm;

	for (int32 Y = 0; Y < CountY; ++Y)
	{
		for (int32 X = 0; X < CountX; ++X)
		{
			const FVector2D Centre = CentreOf(X, Y);
			const int32 At = Index(X, Y);

			const double Distance =
				ARPGFluidGeometry::PolygonSignedDistance(Ring, Centre);

			Edge[At] = Clamped(FMath::Clamp(
				static_cast<float>(Distance) * ToMm, -Band, Band));

			// STILL A WHOLE CELL OR NONE. The material is uniform right up to the
			// outline and then stops -- a fresh slab has a vertical edge, not a
			// bevel -- so what the distance above buys is where the mesher CUTS,
			// not a thickness that fades out. Filling by cell centre keeps every
			// volume, area and buoyancy answer exactly what it was.
			if (Distance <= 0.0)
			{
				Top[At] = Surface;
			}
		}
	}

	Refresh();
}

bool FARPGSolidField::Resolidify(const TArray<FVector2D>& Ring, float SurfaceZ, float MinimumGain)
{
	if (!IsValidField() || Ring.Num() < 3)
	{
		return false;
	}

	const int16 NewTop = Clamped(SurfaceZ * ToMm);
	const float Band = CellSize * ToMm;
	const bool bHasEdge = Edge.Num() == Top.Num();
	bool bGained = false;

	for (int32 Y = 0; Y < CountY; ++Y)
	{
		for (int32 X = 0; X < CountX; ++X)
		{
			const double Distance =
				ARPGFluidGeometry::PolygonSignedDistance(Ring, CentreOf(X, Y));

			// THE OUTLINE GROWS WITH THE SLAB, as the smaller of the two distances
			// -- which is what a union of two regions is when each is written as a
			// distance to its own edge. Ice refreezing around a floe genuinely
			// extends the shape, and leaving the old outline in place would have
			// the mesher cut the new material off at the line the ORIGINAL freeze
			// stopped at.
			if (bHasEdge)
			{
				const int16 Now = Clamped(FMath::Clamp(
					static_cast<float>(Distance) * ToMm, -Band, Band));

				Edge[Index(X, Y)] = FMath::Min(Edge[Index(X, Y)], Now);
			}

			if (Distance > 0.0)
			{
				continue;
			}

			const int32 At = Index(X, Y);

			// WHAT IS ALREADY PROUD OF THE WATERLINE IS NOT ADDED TO. New material forms at
			// the surface of the water, and where the slab already stands above
			// that there is no water to freeze -- which is precisely why a floe
			// pushed down under a load gains its new material BELOW the old.
			if (Top[At] >= NewTop)
			{
				continue;
			}

			if ((NewTop - Top[At]) * ToCm < MinimumGain)
			{
				continue;
			}

			Top[At] = NewTop;
			bGained = true;
		}
	}

	if (bGained)
	{
		Refresh();
	}

	return bGained;
}

double FARPGSolidField::MeltBowl(const FVector2D& At, float Radius, float Depth)
{
	if (!IsValidField() || Radius <= 0.f || Depth <= 0.f)
	{
		return 0.0;
	}

	const double CellArea = static_cast<double>(CellSize) * CellSize;
	const float RadiusSq = Radius * Radius;
	double Removed = 0.0;

	// Only the cells the impact can reach. This is the whole cost of a fireball
	// landing on a floe -- no boolean, no offsetter, no outline to retriangulate.
	const FIntPoint Low = CellAt(At - FVector2D(Radius, Radius));
	const FIntPoint High = CellAt(At + FVector2D(Radius, Radius));

	const int32 MinX = FMath::Max(0, Low.X >= 0 ? Low.X : 0);
	const int32 MinY = FMath::Max(0, Low.Y >= 0 ? Low.Y : 0);
	const int32 MaxX = High.X >= 0 ? High.X : CountX - 1;
	const int32 MaxY = High.Y >= 0 ? High.Y : CountY - 1;

	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			const float DistanceSq = FVector2D::DistSquared(CentreOf(X, Y), At);
			if (DistanceSq > RadiusSq)
			{
				continue;
			}

			// A BOWL, not a cylinder: deepest at the centre, tapering to nothing at
			// the rim. This one line is why a fireball landing at the edge of a floe
			// cuts it away at an angle instead of stamping a bite out of it, and why
			// one landing in the middle leaves a dish that becomes a hole only where
			// it is deep enough to reach the underside.
			const float Falloff = 1.f - DistanceSq / RadiusSq;
			const float Cut = Depth * Falloff;

			const int32 Cell = Index(X, Y);
			const float Before = ThicknessAt(X, Y);
			if (Before <= 0.f)
			{
				continue;
			}

			Top[Cell] = Clamped(FMath::Max(
				static_cast<float>(Bottom[Cell]), Top[Cell] - Cut * ToMm));

			Removed += (Before - ThicknessAt(X, Y)) * CellArea;
		}
	}

	if (Removed > 0.0)
	{
		Refresh();
	}

	return Removed;
}

void FARPGSolidField::RebuildBand()
{
	if (!IsValidField() || Edge.Num() != Top.Num())
	{
		return;
	}

	const float Band = CellSize * ToMm;

	// HALF A CELL is where the boundary between a solid cell and an empty one
	// actually runs, and half a cell is also all the precision a plane clamped to
	// one cell either side can carry. So the rebuilt band is deliberately
	// cell-quantised: it is the honest reading of the occupancy it was derived
	// from, and it is only ever reached once erosion has taken a whole cell --
	// see Erode, which leaves the outline the slab froze with alone until then.
	const int16 Rim = Clamped(CellSize * 0.5f * ToMm);

	static const FIntPoint Neighbours[4] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };

	TArray<int16> Rebuilt;
	Rebuilt.SetNumUninitialized(Edge.Num());

	for (int32 Y = 0; Y < CountY; ++Y)
	{
		for (int32 X = 0; X < CountX; ++X)
		{
			const bool bSolid = IsSolid(X, Y);
			bool bBorders = false;

			for (const FIntPoint& Step : Neighbours)
			{
				const int32 NX = X + Step.X;
				const int32 NY = Y + Step.Y;

				const bool bNeighbourSolid = NX >= 0 && NY >= 0
					&& NX < CountX && NY < CountY && IsSolid(NX, NY);

				bBorders |= bNeighbourSolid != bSolid;
			}

			Rebuilt[Index(X, Y)] = bBorders
				? (bSolid ? -Rim : Rim)
				: Clamped(bSolid ? -Band : Band);
		}
	}

	Edge = MoveTemp(Rebuilt);
}

double FARPGSolidField::Erode(float Distance)
{
	// NO OUTLINE, NOTHING TO PULL IN. A field with no Edge plane is meshed by the
	// cell rule and has no sub-cell boundary to move -- see InsetAt.
	if (!IsValidField() || Distance <= 0.f || Edge.Num() != Top.Num())
	{
		return 0.0;
	}

	const double CellArea = static_cast<double>(CellSize) * CellSize;
	const float Band = CellSize * ToMm;
	const int16 Step = Clamped(Distance * ToMm);

	double Removed = 0.0;

	for (int32 Cell = 0; Cell < Top.Num(); ++Cell)
	{
		// ONLY WHAT THE BAND CAN SEE. Edge is clamped to one cell either side, so
		// everything further in than that is saturated at the same value and knows
		// only "deep inside" -- and adding to a saturated cell is how a single
		// erosion of more than one cell took the entire slab at once. The interior
		// waits its turn; the band comes to it as the rim goes.
		if (Edge[Cell] <= -Band)
		{
			continue;
		}

		// POSITIVE IS OUTSIDE, so adding moves the cell further out and the zero
		// crossing -- which is where the mesher cuts -- travels inward.
		Edge[Cell] = Clamped(FMath::Clamp(
			static_cast<float>(Edge[Cell]) + Step, -Band, Band));

		if (Edge[Cell] < 0)
		{
			continue;
		}

		// AND THE CELL ITSELF GOES WITH IT once its centre is outside. The drawn
		// contour is sub-cell, but area, volume, buoyancy and standing all read
		// whole cells -- so leaving the material here would have a floe keep its
		// weight and its footing well past the edge it is drawn with.
		const float Before = FMath::Max(0.f, (Top[Cell] - Bottom[Cell]) * ToCm);
		if (Before <= 0.f)
		{
			continue;
		}

		Top[Cell] = Bottom[Cell];
		Removed += Before * CellArea;
	}

	if (Removed > 0.0)
	{
		// THE RIM MOVED A WHOLE CELL, so the band has to move with it -- the cells
		// behind the ones just taken are the new edge and are still saturated at
		// "deep inside". Only done when something was actually removed, so a floe
		// that is merely creeping inward within its band keeps the precise outline
		// it froze with.
		RebuildBand();
		Refresh();
	}

	return Removed;
}

double FARPGSolidField::MeltUniform(float FromTop, float FromBottom)
{
	if (!IsValidField() || (FromTop <= 0.f && FromBottom <= 0.f))
	{
		return 0.0;
	}

	const double CellArea = static_cast<double>(CellSize) * CellSize;
	double Removed = 0.0;

	for (int32 Cell = 0; Cell < Top.Num(); ++Cell)
	{
		const float Before = FMath::Max(0.f, (Top[Cell] - Bottom[Cell]) * ToCm);
		if (Before <= 0.f)
		{
			continue;
		}

		// FROM BOTH FACES. A floe in water melts from underneath as much as from
		// above, and keeping the two separate is what lets a slab thin out and
		// vanish rather than only ever being eroded from the top down.
		const int16 NewTop = Clamped(Top[Cell] - FromTop * ToMm);
		const int16 NewBottom = Clamped(Bottom[Cell] + FromBottom * ToMm);

		if (NewTop <= NewBottom)
		{
			// THE TWO FACES MET, so the cell is GONE -- both parked at the height
			// they met at, which is what a hole is. Moving only the top left the
			// cell a sliver exactly as thick as this step's melt from below, so a
			// slab thinning uniformly never actually melted through: it shed the
			// same sliver every tick forever, reported having removed it every
			// time, and no floe left in the sun ever went away.
			const int16 Met = FMath::Clamp(NewBottom, Bottom[Cell], Top[Cell]);

			Top[Cell] = Met;
			Bottom[Cell] = Met;
			Removed += Before * CellArea;
			continue;
		}

		Top[Cell] = NewTop;
		Bottom[Cell] = NewBottom;

		Removed += (Before - FMath::Max(0.f, (NewTop - NewBottom) * ToCm)) * CellArea;
	}

	if (Removed > 0.0)
	{
		Refresh();
	}

	return Removed;
}
