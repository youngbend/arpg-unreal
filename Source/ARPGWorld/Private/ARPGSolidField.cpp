// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGSolidField.h"
#include "ARPGFluidGeometry.h"

namespace
{
	/** Millimetres to centimetres and back -- see the field's storage note. */
	constexpr float ToCm = 0.1f;
	constexpr float ToMm = 10.f;

	/** Below this a cell is bare water rather than very thin ice, in cm. */
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

bool FARPGSolidField::IsIced(int32 X, int32 Y) const
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

bool FARPGSolidField::IsIcedAt(const FVector2D& World) const
{
	const FIntPoint Cell = CellAt(World);
	return Cell.X >= 0 && IsIced(Cell.X, Cell.Y);
}

float FARPGSolidField::TopAt(const FVector2D& World) const
{
	const FIntPoint Cell = CellAt(World);
	return Cell.X >= 0 ? Top[Index(Cell.X, Cell.Y)] * ToCm : 0.f;
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
			if (!IsIced(X, Y))
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
}

double FARPGSolidField::SupportDistance(const FVector2D& From, const FVector2D& Direction) const
{
	double Furthest = 0.0;

	for (int32 Y = 0; Y < CountY; ++Y)
	{
		for (int32 X = 0; X < CountX; ++X)
		{
			if (IsIced(X, Y))
			{
				Furthest = FMath::Max(Furthest,
					FVector2D::DotProduct(CentreOf(X, Y) - From, Direction));
			}
		}
	}

	// Half a cell, because the support is measured to cell CENTRES and the ice in
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
	CountX = 0;
	CountY = 0;

	if (Ring.Num() < 3 || InCellSize <= 0.f || Thickness <= 0.f)
	{
		return;
	}

	CellSize = InCellSize;

	// One cell of margin, so the ice never sits flush against the edge of its own
	// grid -- refreezing can add to a floe and would have nowhere to put it.
	const FBox2D Bounds = ARPGFluidGeometry::PolygonBounds(Ring).ExpandBy(CellSize);

	Origin = Bounds.Min;
	CountX = FMath::CeilToInt32(Bounds.GetSize().X / CellSize) + 1;
	CountY = FMath::CeilToInt32(Bounds.GetSize().Y / CellSize) + 1;

	Top.SetNumZeroed(CountX * CountY);
	Bottom.SetNumZeroed(CountX * CountY);

	// ZERO IS THE UNDERSIDE the slab started at, and everything vertical is
	// relative to it -- which is what lets the whole field ride up and down on the
	// buoyancy without a single cell being rewritten.
	const int16 Surface = Clamped(Thickness * ToMm);

	for (int32 Y = 0; Y < CountY; ++Y)
	{
		for (int32 X = 0; X < CountX; ++X)
		{
			if (ARPGFluidGeometry::PolygonContains(Ring, CentreOf(X, Y)))
			{
				Top[Index(X, Y)] = Surface;
			}
		}
	}

	Refresh();
}

bool FARPGSolidField::Refreeze(const TArray<FVector2D>& Ring, float WaterlineZ, float MinimumGain)
{
	if (!IsValidField() || Ring.Num() < 3)
	{
		return false;
	}

	const int16 NewTop = Clamped(WaterlineZ * ToMm);
	bool bGained = false;

	for (int32 Y = 0; Y < CountY; ++Y)
	{
		for (int32 X = 0; X < CountX; ++X)
		{
			if (!ARPGFluidGeometry::PolygonContains(Ring, CentreOf(X, Y)))
			{
				continue;
			}

			const int32 At = Index(X, Y);

			// ICE ALREADY PROUD OF THE WATERLINE IS NOT ADDED TO. New ice forms at
			// the surface of the water, and where the slab already stands above
			// that there is no water to freeze -- which is precisely why a floe
			// pushed down under a load gains its new ice BELOW the old.
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
			Top[Cell] = NewBottom;
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
