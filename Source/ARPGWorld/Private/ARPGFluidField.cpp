// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGFluidField.h"

#include "Async/ParallelFor.h"

#include "ARPGFluidGeometry.h"

namespace
{
	/**
	 * Floor division that stays floor across zero.
	 *
	 * C++ TRUNCATES TOWARDS ZERO and a grid does not. -1 / 96 is 0 by the
	 * language and -1 by the geometry, so every cell in the chunk west of the
	 * origin would land in the chunk east of it and the whole field would mirror
	 * about the axis. The same trap ResolveCell's positive-local-offset comment
	 * warns about in the spread solver, one level lower down.
	 */
	FORCEINLINE int32 FloorDiv(int32 A, int32 B)
	{
		return (A >= 0) ? (A / B) : -(((-A) + B - 1) / B);
	}

	/** The four face neighbours. Corners are not neighbours -- see FindRegions. */
	constexpr int32 NeighbourDX[4] = { 1, -1, 0, 0 };
	constexpr int32 NeighbourDY[4] = { 0, 0, 1, -1 };
}

// ---------------------------------------------------------------------------
// Chunks

void FARPGFluidChunk::Init(int32 SlotCount)
{
	const int32 CellCount = SlotCount;

	Depth.SetNumZeroed(CellCount);
	VelX.SetNumZeroed(CellCount);
	VelY.SetNumZeroed(CellCount);
	Bed.SetNumZeroed(CellCount);
	SolidTop.SetNumZeroed(CellCount);
	Flags.SetNumZeroed(CellCount);
	ActiveStamp.SetNumZeroed(CellCount);
	Delta.SetNumZeroed(CellCount);
	NetFluxX.SetNumZeroed(CellCount);
	NetFluxY.SetNumZeroed(CellCount);
	TouchStamp.SetNumZeroed(CellCount);

	Active.Reset();
	NextActive.Reset();
	Touched.Reset();
	WetCells = 0;
}

// ---------------------------------------------------------------------------
// Setup

void FARPGFluidField::Configure(float InChunkSize, int32 InResolution,
	const FARPGFluidFieldParams& InParams, int32 InLayers)
{
	// A RESIZE REINTERPRETS EVERY INDEX, so this is a hard error rather than a
	// quiet rebuild: the cells would keep their numbers and change their meaning,
	// and every puddle in the world would be somewhere else by an amount nobody
	// could work out from the symptom.
	checkf(Chunks.Num() == 0, TEXT("FARPGFluidField::Configure on a field with water in it"));

	ChunkSize = FMath::Max(1.f, InChunkSize);
	Resolution = FMath::Clamp(InResolution, 4, 512);
	Layers = FMath::Clamp(InLayers, 1, 4);
	Params = InParams;
}

void FARPGFluidField::Reset()
{
	Chunks.Reset();
	CrossDeposits.Reset();
	Wet.Reset();
}

// ---------------------------------------------------------------------------
// Addressing

FIntPoint FARPGFluidField::GlobalCellAt(const FVector2D& World) const
{
	const float Cell = GetCellSize();
	return FIntPoint(
		FMath::FloorToInt(World.X / Cell),
		FMath::FloorToInt(World.Y / Cell));
}

void FARPGFluidField::ResolveGlobal(int32 GlobalX, int32 GlobalY,
	FIntPoint& OutCoord, int32& OutIndex) const
{
	OutCoord = FIntPoint(FloorDiv(GlobalX, Resolution), FloorDiv(GlobalY, Resolution));

	const int32 LocalX = GlobalX - OutCoord.X * Resolution;
	const int32 LocalY = GlobalY - OutCoord.Y * Resolution;

	OutIndex = LocalY * Resolution + LocalX;
}

void FARPGFluidField::ResolveCell(const FVector2D& World, FIntPoint& OutCoord, int32& OutIndex) const
{
	const FIntPoint Global = GlobalCellAt(World);
	ResolveGlobal(Global.X, Global.Y, OutCoord, OutIndex);
}

FVector2D FARPGFluidField::CellCentre(FIntPoint Coord, int32 Index) const
{
	const float Cell = GetCellSize();

	// TAKES A SLOT OR A CELL, and answers the same either way. Every caller that
	// walks the grid holds slots, and the layer says nothing about where a cell is
	// -- two floors are the same square of ground seen twice.
	const int32 Local = CellOfSlot(Index);
	const int32 LocalX = Local % Resolution;
	const int32 LocalY = Local / Resolution;

	return FVector2D(
		(Coord.X * Resolution + LocalX + 0.5f) * Cell,
		(Coord.Y * Resolution + LocalY + 0.5f) * Cell);
}

void FARPGFluidField::StepCell(FIntPoint Coord, int32 Index, int32 DX, int32 DY,
	FIntPoint& OutCoord, int32& OutIndex) const
{
	const int32 Layer = LayerOfSlot(Index);
	const int32 Local = CellOfSlot(Index);

	const int32 GlobalX = Coord.X * Resolution + (Local % Resolution) + DX;
	const int32 GlobalY = Coord.Y * Resolution + (Local / Resolution) + DY;

	int32 Cell = 0;
	ResolveGlobal(GlobalX, GlobalY, OutCoord, Cell);

	// THE SAME LAYER BY DEFAULT, which is a guess the caller is expected to
	// correct. Layer numbers are allocation order, not height -- cell A's first
	// floor may be cell B's second -- so anything that actually cares which floor
	// it is stepping onto asks FindLayerNear about the bed it is standing on. The
	// solver does; a bounds walk does not need to.
	OutIndex = SlotOf(Layer, Cell);
}

FARPGFluidChunk* FARPGFluidField::FindChunk(FIntPoint Coord)
{
	return Chunks.Find(Coord);
}

const FARPGFluidChunk* FARPGFluidField::FindChunk(FIntPoint Coord) const
{
	return Chunks.Find(Coord);
}

FARPGFluidChunk& FARPGFluidField::FindOrAddChunk(FIntPoint Coord)
{
	if (FARPGFluidChunk* Existing = Chunks.Find(Coord))
	{
		return *Existing;
	}

	FARPGFluidChunk& Chunk = Chunks.Add(Coord);
	Chunk.Init(GetCellCount() * Layers);
	return Chunk;
}

int32 FARPGFluidField::FindLayerNear(const FARPGFluidChunk& Chunk, int32 Cell, float NearZ) const
{
	int32 Best = INDEX_NONE;
	float BestGap = Params.LayerSeparation;

	for (int32 Layer = 0; Layer < Layers; ++Layer)
	{
		const int32 Slot = SlotOf(Layer, Cell);

		if (!EnumHasAnyFlags(Chunk.Flags[Slot], EARPGFluidCellFlags::BedKnown))
		{
			continue;
		}

		const float Gap = FMath::Abs(Chunk.Bed[Slot] - NearZ);

		if (Gap <= BestGap)
		{
			BestGap = Gap;
			Best = Layer;
		}
	}

	return Best;
}

int32 FARPGFluidField::FindWetLayer(const FARPGFluidChunk& Chunk, int32 Cell) const
{
	int32 Best = INDEX_NONE;
	float Most = 0.f;

	for (int32 Layer = 0; Layer < Layers; ++Layer)
	{
		const int32 Slot = SlotOf(Layer, Cell);

		if (Chunk.Depth[Slot] > Most)
		{
			Most = Chunk.Depth[Slot];
			Best = Layer;
		}
	}

	// Nothing wet: the lowest floor anybody has bothered to trace for is the
	// honest answer to "which surface is this", and is what a bed query wants.
	if (Best == INDEX_NONE)
	{
		float Lowest = TNumericLimits<float>::Max();

		for (int32 Layer = 0; Layer < Layers; ++Layer)
		{
			const int32 Slot = SlotOf(Layer, Cell);

			if (EnumHasAnyFlags(Chunk.Flags[Slot], EARPGFluidCellFlags::BedKnown)
				&& Chunk.Bed[Slot] < Lowest)
			{
				Lowest = Chunk.Bed[Slot];
				Best = Layer;
			}
		}
	}

	return Best;
}

int32 FARPGFluidField::EnsureLayer(FIntPoint Coord, int32 Cell, float NearZ, float& OutBed)
{
	FARPGFluidChunk& Chunk = FindOrAddChunk(Coord);

	// ALREADY KNOWN, and near enough to be the same floor.
	if (const int32 Known = FindLayerNear(Chunk, Cell, NearZ); Known != INDEX_NONE)
	{
		OutBed = Chunk.Bed[SlotOf(Known, Cell)];
		return EnumHasAnyFlags(Chunk.Flags[SlotOf(Known, Cell)], EARPGFluidCellFlags::NoFloor)
			? INDEX_NONE : Known;
	}

	float Floor = NearZ;
	const bool bFound = BedProbe ? BedProbe(CellCentre(Coord, Cell), Floor) : false;

	// MATCHED AGAINST WHAT THE TRACE FOUND, not against what was asked for -- see
	// the header. Water arriving at the top of a drop asks about the top and the
	// floor is at the bottom, and matching the question rather than the answer
	// grows a second layer holding the same ground as the first.
	if (bFound)
	{
		if (const int32 Same = FindLayerNear(Chunk, Cell, Floor); Same != INDEX_NONE)
		{
			OutBed = Chunk.Bed[SlotOf(Same, Cell)];
			return EnumHasAnyFlags(Chunk.Flags[SlotOf(Same, Cell)], EARPGFluidCellFlags::NoFloor)
				? INDEX_NONE : Same;
		}
	}

	for (int32 Layer = 0; Layer < Layers; ++Layer)
	{
		const int32 Slot = SlotOf(Layer, Cell);

		if (EnumHasAnyFlags(Chunk.Flags[Slot], EARPGFluidCellFlags::BedKnown))
		{
			continue;
		}

		Chunk.Bed[Slot] = Floor;
		Chunk.Flags[Slot] |= EARPGFluidCellFlags::BedKnown;

		// A REFUSAL IS AN ANSWER, and remembering it is the difference between a
		// puddle that stops at a lip and one that hangs over the drop past it. The
		// probe was asked once; asking again every step would be a trace per dry
		// cell per tick around the rim of every body of water in the world.
		if (!bFound)
		{
			Chunk.Flags[Slot] |= EARPGFluidCellFlags::NoFloor;
		}

		OutBed = Floor;
		return bFound ? Layer : INDEX_NONE;
	}

	// EVERY FLOOR TAKEN AND NONE OF THEM THIS ONE. A third surface in one column,
	// which is not a level anybody builds -- treated as a wall, which is the
	// honest way to run out of a resource rather than silently overwriting a floor
	// somebody's puddle is already lying on.
	OutBed = Floor;
	return INDEX_NONE;
}

// ---------------------------------------------------------------------------
// The active frontier

void FARPGFluidField::MarkActive(FARPGFluidChunk& Chunk, int32 Index)
{
	if (!Chunk.ActiveStamp.IsValidIndex(Index) || Chunk.ActiveStamp[Index] != 0)
	{
		return;
	}

	Chunk.ActiveStamp[Index] = 1;
	Chunk.Active.Add(Index);
}

void FARPGFluidField::MarkNextActive(FARPGFluidChunk& Chunk, int32 Index)
{
	if (!Chunk.ActiveStamp.IsValidIndex(Index))
	{
		return;
	}

	// Stamped 2 for "next", so a cell already in this step's list is not added
	// twice and the two sets never alias -- the same two-value stamp the spread
	// solver uses, and for the same reason.
	if (Chunk.ActiveStamp[Index] == 2)
	{
		return;
	}

	Chunk.ActiveStamp[Index] = 2;
	Chunk.NextActive.Add(Index);
}

void FARPGFluidField::WakeAround(FIntPoint Coord, int32 Index)
{
	MarkActive(FindOrAddChunk(Coord), Index);

	for (int32 Dir = 0; Dir < 4; ++Dir)
	{
		FIntPoint NeighbourCoord;
		int32 NeighbourIndex = 0;
		StepCell(Coord, Index, NeighbourDX[Dir], NeighbourDY[Dir], NeighbourCoord, NeighbourIndex);

		MarkActive(FindOrAddChunk(NeighbourCoord), NeighbourIndex);
	}
}

// ---------------------------------------------------------------------------
// Putting fluid in and taking it out

double FARPGFluidField::PourVolume(const FVector2D& Centre, float NearZ, double Volume,
	float MaxRadius)
{
	if (Volume <= 0.0)
	{
		return 0.0;
	}

	const float Cell = GetCellSize();
	const double CellArea = static_cast<double>(Cell) * Cell;

	// HOW WIDE IS THE FLUID'S OWN BUSINESS -- the caller has a volume and no
	// opinion. Its deposit depth is the exchange rate, exactly as
	// UARPGFluidSurfaceSubsystem::ReturnFluid has always used Depth for.
	const double WantArea = Volume / FMath::Max(0.01f, Params.DepositDepth);
	const float WantRadius = FMath::Sqrt(static_cast<float>(WantArea / PI));
	// NO FLOOR ON THE RADIUS. The flood fill always takes the cell it started in,
	// so a pour smaller than a cell still lands -- and a floor of half a cell made
	// a ten-centimetre splash reach its four neighbours as well, five times the
	// ground it asked for and enough to count as a body of its own.
	const float Radius = FMath::Min(WantRadius, FMath::Max(1.f, MaxRadius));

	// GATHERED BY WALKING OUT FROM THE MIDDLE, not by testing each cell against
	// where the pour was aimed. This is the port of ClipToGround, and its comment
	// is the reason: the question is not "is this cell level with where I started"
	// -- down a ramp nothing is -- but "did the floor get here without jumping".
	// So each cell is compared against the NEIGHBOUR it was reached from, which a
	// slope passes however far it runs and a ledge fails at its lip.
	//
	// A grid does it better than the spokes did, too. Spokes could only pull each
	// vertex of a disc inward along its own radius, so a footprint had to stay
	// star-shaped about its centre; a flood fill will happily lay water round a
	// corner and refuse a hole in the middle of the same pour.
	TArray<TPair<FIntPoint, int32>> Targets;

	FIntPoint MiddleCoord;
	int32 MiddleCell = 0;
	ResolveCell(Centre, MiddleCoord, MiddleCell);

	// WHICH FLOOR THIS POUR IS FOR. NearZ is the only thing that says whether a
	// spell that finished over a balcony wet the balcony or the room beneath it,
	// and it is the caller's own answer to that -- the discharge traced down to
	// find it. Everything the flood fill reaches afterwards stays on the floor
	// this one picked, because each step matches the bed it came from.
	float MiddleFloor = NearZ;
	const int32 MiddleLayer = EnsureLayer(MiddleCoord, MiddleCell, NearZ, MiddleFloor);
	const bool bGroundBelow = MiddleLayer != INDEX_NONE;

	// NOTHING UNDER THE MIDDLE means there is no floor to follow -- the caller is
	// laying fluid in a world with no collision under it, which every automation
	// fixture is and which a spell finishing over a hole also is. Taken at its
	// word and laid flat at the height asked for, exactly as ClipToGround handed
	// its footprint back whole in the same case.
	const int32 Layer = bGroundBelow ? MiddleLayer : 0;
	const int32 MiddleIndex = SlotOf(Layer, MiddleCell);

	if (!bGroundBelow)
	{
		FARPGFluidChunk& Middle = Chunks.FindChecked(MiddleCoord);
		Middle.Bed[MiddleIndex] = NearZ;
		Middle.Flags[MiddleIndex] |= EARPGFluidCellFlags::BedKnown;
		Middle.Flags[MiddleIndex] &= ~EARPGFluidCellFlags::NoFloor;
		MiddleFloor = NearZ;
	}

	{
		TSet<TPair<FIntPoint, int32>> Seen;
		TArray<TPair<FIntPoint, int32>> Pending;

		Pending.Emplace(MiddleCoord, MiddleIndex);
		Seen.Add(Pending[0]);

		while (Pending.Num() > 0)
		{
			const TPair<FIntPoint, int32> At = Pending.Pop(EAllowShrinking::No);

			{
				const FARPGFluidChunk& Chunk = Chunks.FindChecked(At.Key);

				if (EnumHasAnyFlags(Chunk.Flags[At.Value], EARPGFluidCellFlags::Blocked))
				{
					continue;
				}
			}

			Targets.Add(At);

			const float Here = Chunks.FindChecked(At.Key).Bed[At.Value];

			for (int32 Dir = 0; Dir < 4; ++Dir)
			{
				FIntPoint NextCoord;
				int32 NextIndex = 0;
				StepCell(At.Key, At.Value, NeighbourDX[Dir], NeighbourDY[Dir],
					NextCoord, NextIndex);

				const TPair<FIntPoint, int32> Next(NextCoord, NextIndex);

				if (Seen.Contains(Next))
				{
					continue;
				}

				if (FVector2D::Distance(CellCentre(NextCoord, NextIndex), Centre) > Radius)
				{
					continue;
				}

				Seen.Add(Next);

				float Floor = 0.f;
				const int32 NextLayer =
					EnsureLayer(NextCoord, CellOfSlot(NextIndex), Here, Floor);

				if (!bGroundBelow)
				{
					// Flat world: everything the disc covers is fair game, and the
					// probe's refusals mean nothing because it refuses everywhere.
					FARPGFluidChunk& Chunk = Chunks.FindChecked(NextCoord);
					Chunk.Bed[NextIndex] = NearZ;
					Chunk.Flags[NextIndex] |= EARPGFluidCellFlags::BedKnown;
					Chunk.Flags[NextIndex] &= ~EARPGFluidCellFlags::NoFloor;
				}
				else if (NextLayer == INDEX_NONE
					|| FMath::Abs(Floor - Here) > Params.MaxDepositDrop)
				{
					// The lip. Not followed, and neither is anything past it.
					continue;
				}
				else if (NextLayer != LayerOfSlot(NextIndex))
				{
					// THE FLOOR IS THERE UNDER A DIFFERENT NUMBER. Layers are
					// allocation order, so the continuation of this cell's floor may
					// be the neighbour's second rather than its first -- and the
					// cell already noted as seen is the wrong slot.
					const TPair<FIntPoint, int32> Corrected(
						NextCoord, SlotOf(NextLayer, CellOfSlot(NextIndex)));

					if (!Seen.Contains(Corrected))
					{
						Seen.Add(Corrected);
						Pending.Add(Corrected);
					}

					continue;
				}

				Pending.Add(Next);
			}
		}
	}

	const double Share = Volume / (Targets.Num() * CellArea);

	for (const TPair<FIntPoint, int32>& Target : Targets)
	{
		FARPGFluidChunk& Chunk = Chunks.FindChecked(Target.Key);

		if (Chunk.Depth[Target.Value] <= 0.f)
		{
			++Chunk.WetCells;
		}

		Chunk.Depth[Target.Value] += static_cast<float>(Share);
		WakeAround(Target.Key, Target.Value);
	}

	return Volume;
}

double FARPGFluidField::RemoveVolume(const FVector2D& Centre, float MaxRadius, double Volume,
	int32 Layer)
{
	if (Volume <= 0.0 || MaxRadius <= 0.f)
	{
		return 0.0;
	}

	const float Cell = GetCellSize();
	const double CellArea = static_cast<double>(Cell) * Cell;

	struct FCandidate
	{
		FIntPoint Coord;
		int32 Index;
		float DistanceSq;
	};

	TArray<FCandidate> Candidates;

	const FIntPoint Min = GlobalCellAt(Centre - FVector2D(MaxRadius, MaxRadius));
	const FIntPoint Max = GlobalCellAt(Centre + FVector2D(MaxRadius, MaxRadius));

	for (int32 GY = Min.Y; GY <= Max.Y; ++GY)
	{
		for (int32 GX = Min.X; GX <= Max.X; ++GX)
		{
			FIntPoint Coord;
			int32 Index = 0;
			ResolveGlobal(GX, GY, Coord, Index);

			const FARPGFluidChunk* Chunk = FindChunk(Coord);

			if (!Chunk || Chunk->Depth[Index] <= 0.f)
			{
				continue;
			}

			const float DistanceSq = static_cast<float>(
				FVector2D::DistSquared(CellCentre(Coord, Index), Centre));

			if (DistanceSq > MaxRadius * MaxRadius)
			{
				continue;
			}

			Candidates.Add({ Coord, Index, DistanceSq });
		}
	}

	// NEAREST FIRST, because what asks for this is a reaction that happened at a
	// point. Spreading the loss evenly over the whole contact disc would take
	// water off the far rim of a puddle a fireball touched the near edge of.
	Candidates.Sort([](const FCandidate& A, const FCandidate& B)
		{
			return A.DistanceSq < B.DistanceSq;
		});

	double Remaining = Volume;

	for (const FCandidate& Candidate : Candidates)
	{
		if (Remaining <= 0.0)
		{
			break;
		}

		FARPGFluidChunk& Chunk = Chunks.FindChecked(Candidate.Coord);

		const double Available = static_cast<double>(Chunk.Depth[Candidate.Index]) * CellArea;
		const double Taken = FMath::Min(Available, Remaining);

		Chunk.Depth[Candidate.Index] -= static_cast<float>(Taken / CellArea);
		Remaining -= Taken;

		if (Chunk.Depth[Candidate.Index] <= 0.f)
		{
			Chunk.Depth[Candidate.Index] = 0.f;
			--Chunk.WetCells;
		}

		WakeAround(Candidate.Coord, Candidate.Index);
	}

	return Volume - Remaining;
}

double FARPGFluidField::ConsumeRegion(const TArray<FVector2D>& Region, int32 Layer)
{
	if (Region.Num() < 3)
	{
		return 0.0;
	}

	const float Cell = GetCellSize();
	const double CellArea = static_cast<double>(Cell) * Cell;
	const FBox2D Bounds = ARPGFluidGeometry::PolygonBounds(Region);

	const FIntPoint Min = GlobalCellAt(Bounds.Min);
	const FIntPoint Max = GlobalCellAt(Bounds.Max);

	double Taken = 0.0;

	for (int32 GY = Min.Y; GY <= Max.Y; ++GY)
	{
		for (int32 GX = Min.X; GX <= Max.X; ++GX)
		{
			FIntPoint Coord;
			int32 Index = 0;
			ResolveGlobal(GX, GY, Coord, Index);

			FARPGFluidChunk* Chunk = FindChunk(Coord);

			if (!Chunk)
			{
				continue;
			}

			// COVERED, NOT CONTAINED, and the half cell is the whole of the
			// difference. A cell is a square and a polygon does not respect it, so
			// a strict centre-inside test leaves every cell the region only partly
			// covers -- which around the rim of a freeze is a ring of water the ice
			// is visibly sitting on.
			//
			// It matters most where the two shapes ought to agree exactly: an
			// agent's circle is a sixteen-sided polygon INSCRIBED in it, so it
			// always falls slightly short of the circle it stands for, and a spell
			// that plainly covered a whole puddle left a rim of it behind.
			if (ARPGFluidGeometry::PolygonSignedDistance(Region, CellCentre(Coord, Index))
				> Cell * 0.5)
			{
				continue;
			}

			// STAMPED WHETHER OR NOT THERE WAS ANYTHING IN IT. What the stamp
			// stops is the surrounding water flowing BACK, and a cell the ice
			// covers needs that whether it was wet a moment ago or not.
			Chunk->Flags[Index] |= EARPGFluidCellFlags::Frozen;

			if (Chunk->Depth[Index] <= 0.f)
			{
				continue;
			}

			Taken += static_cast<double>(Chunk->Depth[Index]) * CellArea;
			Chunk->Depth[Index] = 0.f;
			--Chunk->WetCells;

			WakeAround(Coord, Index);
		}
	}

	return Taken;
}

// ---------------------------------------------------------------------------
// Solids

double FARPGFluidField::MarkSolid(const FVector2D& World, float NearZ, float Bottom, float Top)
{
	FIntPoint Coord;
	int32 Index = 0;
	ResolveCell(World, Coord, Index);

	float BedHeight = 0.f;
	const int32 Layer = EnsureLayer(Coord, CellOfSlot(Index), NearZ, BedHeight);

	if (Layer == INDEX_NONE)
	{
		return 0.0;
	}

	Index = SlotOf(Layer, CellOfSlot(Index));

	FARPGFluidChunk& Chunk = Chunks.FindChecked(Coord);

	Chunk.SolidTop[Index] = FMath::Max(Chunk.SolidTop[Index], Top);
	Chunk.Flags[Index] |= EARPGFluidCellFlags::Roofed;

	// A FLOE LEAVES THE WATER ALONE. Only a solid whose underside has reached the
	// floor stops fluid crossing; one still riding on it has water beneath, which
	// is the whole difference between a floe and a plug.
	if (Bottom > BedHeight + Params.MinimumFilm)
	{
		return 0.0;
	}

	Chunk.Flags[Index] |= EARPGFluidCellFlags::Blocked;

	if (Chunk.Depth[Index] <= 0.f)
	{
		return 0.0;
	}

	const float Cell = GetCellSize();
	const double Displaced = static_cast<double>(Chunk.Depth[Index]) * Cell * Cell;

	Chunk.Depth[Index] = 0.f;
	--Chunk.WetCells;

	WakeAround(Coord, Index);

	return Displaced;
}

void FARPGFluidField::ClearSolids()
{
	for (TPair<FIntPoint, FARPGFluidChunk>& Pair : Chunks)
	{
		FARPGFluidChunk& Chunk = Pair.Value;

		for (int32 Index = 0; Index < Chunk.Flags.Num(); ++Index)
		{
			if (EnumHasAnyFlags(Chunk.Flags[Index], EARPGFluidCellFlags::Blocked))
			{
				// Newly open ground has head all around it and should fill.
				MarkActive(Chunk, Index);
			}

			Chunk.Flags[Index] &= ~(EARPGFluidCellFlags::Blocked | EARPGFluidCellFlags::Roofed);
			Chunk.SolidTop[Index] = 0.f;
		}
	}
}

bool FARPGFluidField::IsBlockedAt(const FVector2D& World, int32 Layer) const
{
	FIntPoint Coord;
	int32 Index = 0;
	ResolveCell(World, Coord, Index);

	const FARPGFluidChunk* Chunk = FindChunk(Coord);

	return Chunk && EnumHasAnyFlags(Chunk->Flags[SlotForQuery(*Chunk, Index, Layer)],
		EARPGFluidCellFlags::Blocked);
}

bool FARPGFluidField::IsRoofedAt(const FVector2D& World, int32 Layer) const
{
	FIntPoint Coord;
	int32 Index = 0;
	ResolveCell(World, Coord, Index);

	const FARPGFluidChunk* Chunk = FindChunk(Coord);

	return Chunk && EnumHasAnyFlags(Chunk->Flags[SlotForQuery(*Chunk, Index, Layer)],
		EARPGFluidCellFlags::Roofed);
}

float FARPGFluidField::SampleSolidTop(const FVector2D& World, int32 Layer) const
{
	FIntPoint Coord;
	int32 Index = 0;
	ResolveCell(World, Coord, Index);

	const FARPGFluidChunk* Chunk = FindChunk(Coord);

	return Chunk ? Chunk->SolidTop[SlotForQuery(*Chunk, Index, Layer)] : 0.f;
}

// ---------------------------------------------------------------------------
// Queries

int32 FARPGFluidField::SlotForQuery(const FARPGFluidChunk& Chunk, int32 Cell, int32 Layer) const
{
	// A LAYER GIVEN IS A LAYER MEANT. A proxy knows which floor its body is on and
	// says so; anything else is asking about "the water here", and the honest
	// answer to that is whichever floor has any.
	if (Layer >= 0 && Layer < Layers)
	{
		return SlotOf(Layer, Cell);
	}

	const int32 Holding = FindWetLayer(Chunk, Cell);
	return SlotOf(Holding == INDEX_NONE ? 0 : Holding, Cell);
}

//
// BILINEAR THROUGHOUT, and for the reason the spread mask's own comment gives:
// read per cell, a puddle's edge steps along cell boundaries and the same puddle
// answers differently depending on where you stand relative to one, which reads
// as the simulation being unreliable rather than as aliasing. Interpolating is
// also what lets a forty-centimetre grid carry a surface a character can stand
// on without visible stairs.

float FARPGFluidField::SampleDepth(const FVector2D& World, int32 Layer) const
{
	const float Cell = GetCellSize();

	// Shifted half a cell so the containing cell is the lower-left of the four
	// whose CENTRES bracket the point, rather than the one it happens to sit in.
	const float GX = World.X / Cell - 0.5f;
	const float GY = World.Y / Cell - 0.5f;

	const int32 IX = FMath::FloorToInt(GX);
	const int32 IY = FMath::FloorToInt(GY);
	const float FX = GX - IX;
	const float FY = GY - IY;

	float Corner[4] = {};

	for (int32 Offset = 0; Offset < 4; ++Offset)
	{
		FIntPoint Coord;
		int32 Index = 0;
		ResolveGlobal(IX + (Offset & 1), IY + (Offset >> 1), Coord, Index);

		if (const FARPGFluidChunk* Chunk = FindChunk(Coord))
		{
			Corner[Offset] = Chunk->Depth[SlotForQuery(*Chunk, Index, Layer)];
		}
	}

	return FMath::Lerp(
		FMath::Lerp(Corner[0], Corner[1], FX),
		FMath::Lerp(Corner[2], Corner[3], FX),
		FY);
}

float FARPGFluidField::SampleBed(const FVector2D& World, int32 Layer) const
{
	FIntPoint Coord;
	int32 Index = 0;
	ResolveCell(World, Coord, Index);

	const FARPGFluidChunk* Chunk = FindChunk(Coord);

	if (Chunk)
	{
		const int32 Slot = SlotForQuery(*Chunk, Index, Layer);

		if (EnumHasAnyFlags(Chunk->Flags[Slot], EARPGFluidCellFlags::BedKnown))
		{
			return Chunk->Bed[Slot];
		}
	}

	// NEVER SAMPLED IS NOT THE SAME AS FLAT. Asking the probe is the honest
	// answer for ground the field has simply not needed yet, and it is what stops
	// a query beside a puddle reporting the puddle's own height as the floor.
	float Height = 0.f;

	if (BedProbe)
	{
		BedProbe(World, Height);
	}

	return Height;
}

float FARPGFluidField::SampleLevel(const FVector2D& World, int32 Layer) const
{
	return SampleBed(World, Layer) + SampleDepth(World, Layer);
}

FVector2D FARPGFluidField::SampleVelocity(const FVector2D& World, int32 Layer) const
{
	FIntPoint Coord;
	int32 Index = 0;
	ResolveCell(World, Coord, Index);

	const FARPGFluidChunk* Chunk = FindChunk(Coord);

	if (!Chunk)
	{
		return FVector2D::ZeroVector;
	}

	const int32 Slot = SlotForQuery(*Chunk, Index, Layer);
	return FVector2D(Chunk->VelX[Slot], Chunk->VelY[Slot]);
}

bool FARPGFluidField::IsWetAt(const FVector2D& World, int32 Layer) const
{
	FIntPoint Coord;
	int32 Index = 0;
	ResolveCell(World, Coord, Index);

	const FARPGFluidChunk* Chunk = FindChunk(Coord);

	return Chunk && Chunk->Depth[SlotForQuery(*Chunk, Index, Layer)] > 0.f;
}

double FARPGFluidField::GetTotalVolume() const
{
	const float Cell = GetCellSize();
	const double CellArea = static_cast<double>(Cell) * Cell;

	double Total = 0.0;

	for (const TPair<FIntPoint, FARPGFluidChunk>& Pair : Chunks)
	{
		for (float Depth : Pair.Value.Depth)
		{
			Total += Depth;
		}
	}

	return Total * CellArea;
}

double FARPGFluidField::GetVolumeWithin(const FVector2D& Centre, float Radius, int32 Layer) const
{
	if (Radius <= 0.f)
	{
		return 0.0;
	}

	const float Cell = GetCellSize();
	const double CellArea = static_cast<double>(Cell) * Cell;

	const FIntPoint Min = GlobalCellAt(Centre - FVector2D(Radius, Radius));
	const FIntPoint Max = GlobalCellAt(Centre + FVector2D(Radius, Radius));

	double Total = 0.0;

	for (int32 GY = Min.Y; GY <= Max.Y; ++GY)
	{
		for (int32 GX = Min.X; GX <= Max.X; ++GX)
		{
			FIntPoint Coord;
			int32 Index = 0;
			ResolveGlobal(GX, GY, Coord, Index);

			const FARPGFluidChunk* Chunk = FindChunk(Coord);

			if (!Chunk)
			{
				continue;
			}

			if (FVector2D::DistSquared(CellCentre(Coord, Index), Centre) > Radius * Radius)
			{
				continue;
			}

			// EVERY FLOOR, NOT JUST THE GROUND FLOOR. Index is a CELL; depth is stored
			// per SLOT, which is a cell on a layer -- so Depth[Index] reads layer zero
			// and nothing else, and the Layer argument was accepted and ignored.
			//
			// WHAT IT COST: UARPGFluidPresentationSubsystem only creates a sheet for an
			// element this says has volume near the viewer. Water that settled on an
			// upper floor answered zero here, so it was never drawn -- while every
			// other query, and the reaction solver, could see it perfectly well. A
			// puddle you can freeze, stand in and walk through, and cannot see.
			for (int32 On = 0; On < Layers; ++On)
			{
				if (Layer == INDEX_NONE || On == Layer)
				{
					Total += static_cast<double>(Chunk->Depth[SlotOf(On, Index)]) * CellArea;
				}
			}
		}
	}

	return Total;
}

double FARPGFluidField::GetWetAreaWithin(const FVector2D& Centre, float Radius, int32 Layer) const
{
	if (Radius <= 0.f)
	{
		return 0.0;
	}

	const float Cell = GetCellSize();
	const double CellArea = static_cast<double>(Cell) * Cell;

	const FIntPoint Min = GlobalCellAt(Centre - FVector2D(Radius, Radius));
	const FIntPoint Max = GlobalCellAt(Centre + FVector2D(Radius, Radius));

	double Total = 0.0;

	for (int32 GY = Min.Y; GY <= Max.Y; ++GY)
	{
		for (int32 GX = Min.X; GX <= Max.X; ++GX)
		{
			FIntPoint Coord;
			int32 Index = 0;
			ResolveGlobal(GX, GY, Coord, Index);

			const FARPGFluidChunk* Chunk = FindChunk(Coord);

			if (!Chunk)
			{
				continue;
			}

			if (FVector2D::DistSquared(CellCentre(Coord, Index), Centre) > Radius * Radius)
			{
				continue;
			}

			// AREA, SO A CELL COUNTS ONCE however many floors of it are wet -- see
			// GetVolumeWithin for why the layer walk is here at all.
			bool bWet = false;

			for (int32 On = 0; On < Layers && !bWet; ++On)
			{
				bWet = (Layer == INDEX_NONE || On == Layer)
					&& Chunk->Depth[SlotOf(On, Index)] > 0.f;
			}

			if (bWet)
			{
				Total += CellArea;
			}
		}
	}

	return Total;
}

int32 FARPGFluidField::GetWetCellCount() const
{
	int32 Count = 0;

	for (const TPair<FIntPoint, FARPGFluidChunk>& Pair : Chunks)
	{
		Count += Pair.Value.WetCells;
	}

	return Count;
}

// ---------------------------------------------------------------------------
// The solver
void FARPGFluidField::Step(float DeltaTime)
{
	if (DeltaTime <= 0.f)
	{
		return;
	}

	const float Cell = GetCellSize();

	// FLATTENED FIRST, because a TMap cannot be indexed and because the sweep must
	// not be able to grow the map underneath itself -- a rehash would invalidate
	// every pointer in this list. Anything landing in a chunk that does not exist
	// yet is posted to CrossDeposits and folded in afterwards.
	Wet.Reset();

	for (TPair<FIntPoint, FARPGFluidChunk>& Pair : Chunks)
	{
		if (Pair.Value.Active.Num() > 0)
		{
			Wet.Emplace(Pair.Key, &Pair.Value);
		}
	}

	CrossDeposits.Reset();

	// CLEARED OVER EVERY CHUNK, NOT JUST THE WET ONES. A chunk can receive a delta
	// from its neighbour without having a single active cell of its own, and if
	// the clear only covered the chunks that were busy, that delta would sit in
	// the array until the chunk happened to wake up -- and then be applied, once,
	// out of an arbitrary number of steps ago. Water from the past.
	for (TPair<FIntPoint, FARPGFluidChunk>& Pair : Chunks)
	{
		FARPGFluidChunk& Chunk = Pair.Value;

		for (int32 Index : Chunk.Touched)
		{
			Chunk.Delta[Index] = 0.f;
			Chunk.NetFluxX[Index] = 0.f;
			Chunk.NetFluxY[Index] = 0.f;
			Chunk.TouchStamp[Index] = 0;
		}

		Chunk.Touched.Reset();
	}

	if (Wet.Num() == 0)
	{
		return;
	}

	// A local, so every place that writes a delta agrees about the bookkeeping.
	auto AddDelta = [](FARPGFluidChunk& Chunk, int32 Index, float Amount)
		{
			if (Chunk.TouchStamp[Index] == 0)
			{
				Chunk.TouchStamp[Index] = 1;
				Chunk.Touched.Add(Index);
			}

			Chunk.Delta[Index] += Amount;
		};

	// --- Sweep -------------------------------------------------------------

	for (TPair<FIntPoint, FARPGFluidChunk*>& Entry : Wet)
	{
		const FIntPoint Coord = Entry.Key;
		FARPGFluidChunk& Chunk = *Entry.Value;

		for (int32 Index : Chunk.Active)
		{
			if (EnumHasAnyFlags(Chunk.Flags[Index], EARPGFluidCellFlags::Blocked))
			{
				continue;
			}

			const float Depth = Chunk.Depth[Index];

			// TOO THIN TO MOVE, NOT TOO THIN TO EXIST -- see MinimumFilm. This is
			// what ends the asymptote without losing the water.
			if (Depth < Params.MinimumFilm)
			{
				continue;
			}

			const float Level = Chunk.Bed[Index] + Depth;

			// HEADS FIRST, MOVES SECOND, and the two passes are not an accident of
			// style. How much a cell may give away in one direction depends on how
			// many OTHER directions it is also giving away in -- see the cap below
			// -- so nothing can be decided until every direction has been measured.
			float Head[4] = {};
			int32 Downhill = 0;

			// Where each direction's flux actually lands, worked out once here and
			// reused below -- see the layer note inside.
			int32 Slot[4] = { INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE };
			float NeighbourBeds[4] = {};

			for (int32 Dir = 0; Dir < 4; ++Dir)
			{
				FIntPoint NeighbourCoord;
				int32 NeighbourIndex = 0;
				StepCell(Coord, Index, NeighbourDX[Dir], NeighbourDY[Dir],
					NeighbourCoord, NeighbourIndex);

				// WHICH OF THE NEIGHBOUR'S FLOORS IS THE CONTINUATION OF THIS ONE.
				// Layer numbers are allocation order and mean nothing across cells
				// -- a balcony may be layer 0 here and layer 1 one step over,
				// depending only on which puddle happened to arrive first. What
				// makes two slots the same surface is that their BEDS are within a
				// LayerSeparation of each other, and matching on that is the whole
				// of how a floor stays connected to itself.
				//
				// ALLOCATING IS SAFE ONLY IN A CHUNK THAT EXISTS. Adding a chunk
				// rehashes the map the sweep is holding pointers into, so a
				// neighbour in unallocated ground is probed and nothing more; the
				// flux that lands there is posted as a cross deposit carrying the
				// bed it found, and the apply pass allocates.
				FARPGFluidChunk* NeighbourChunk = FindChunk(NeighbourCoord);

				if (NeighbourChunk)
				{
					float MatchedBed = 0.f;
					const int32 MatchedLayer = EnsureLayer(NeighbourCoord,
						CellOfSlot(NeighbourIndex), Chunk.Bed[Index], MatchedBed);

					if (MatchedLayer == INDEX_NONE)
					{
						continue;
					}

					NeighbourIndex = SlotOf(MatchedLayer, CellOfSlot(NeighbourIndex));
				}

				Slot[Dir] = NeighbourIndex;

				const FARPGFluidChunk* Neighbour = NeighbourChunk;

				if (Neighbour && EnumHasAnyFlags(Neighbour->Flags[NeighbourIndex],
					EARPGFluidCellFlags::Blocked | EARPGFluidCellFlags::Frozen
					| EARPGFluidCellFlags::NoFloor))
				{
					// A WALL, AND A FREE-SLIP ONE. Nothing crosses, and nothing is
					// lost either -- the outflow is simply not computed, so the water
					// stays where it is and piles up against the face. That is the
					// whole of "flow around a solid".
					continue;
				}

				float NeighbourBed = 0.f;
				float NeighbourDepth = 0.f;

				if (Neighbour && EnumHasAnyFlags(Neighbour->Flags[NeighbourIndex],
					EARPGFluidCellFlags::BedKnown))
				{
					NeighbourBed = Neighbour->Bed[NeighbourIndex];
					NeighbourDepth = Neighbour->Depth[NeighbourIndex];
				}
				else
				{
					// UNALLOCATED IS NOT UNKNOWN. The probe answers for ground the
					// field has never had reason to touch, which is most of the
					// world, and a refusal means there is no floor there at all -- a
					// lip over nothing. Treated as a wall, because the alternative is
					// water pouring off the edge of the level and out of the
					// simulation, which is mass loss dressed up as a feature.
					if (!BedProbe)
					{
						continue;
					}

					if (!BedProbe(CellCentre(NeighbourCoord, NeighbourIndex), NeighbourBed))
					{
						continue;
					}
				}

				const float Difference = Level - (NeighbourBed + NeighbourDepth);

				// THE YIELD SLOPE IS WHAT MAKES LAVA LAVA. Below it nothing moves at
				// all, so a viscous fluid stands in a heap where water would have
				// sheeted away.
				if (Difference <= Params.YieldSlope)
				{
					continue;
				}

				Head[Dir] = Difference - Params.YieldSlope;
				NeighbourBeds[Dir] = NeighbourBed;
				++Downhill;
			}

			if (Downhill == 0)
			{
				continue;
			}

			// ONE OVER THE NUMBER OF NEIGHBOURS PLUS ONE, and getting this wrong is
			// not a tuning error, it is a different simulation.
			//
			// Give a quarter of the head to each of four neighbours and the cell has
			// given away the WHOLE head while each neighbour rose by a quarter of it
			// -- so the cell ends up below every one of them and they all give it
			// straight back. The field alternates wet and dry cell by cell like a
			// chessboard, which conserves mass perfectly and is nonsense: every
			// second cell reads as dry, a puddle stops being one connected body, and
			// FindRegions returned eight hundred of them for one pour.
			//
			// A cell and the N it feeds settle at their common mean, which is
			// reached by moving Head/(N+1) along each edge. That is the largest
			// share that cannot overshoot, so it is both the stable answer and the
			// fastest stable answer.
			const float Cap = 1.f / (Downhill + 1);

			float Outflow[4] = {};
			float Total = 0.f;

			for (int32 Dir = 0; Dir < 4; ++Dir)
			{
				if (Head[Dir] <= 0.f)
				{
					continue;
				}

				float Move = FMath::Min(Head[Dir] * Params.FlowRate * DeltaTime, Head[Dir] * Cap);

				// AND A WALL IS NOT A SLOPE. Where the neighbour's surface is below
				// this cell's own FLOOR the fluid is not settling across a gradient,
				// it is falling down a face, and what distinguishes water from lava
				// there is a speed rather than a share of the head.
				if (Level - Head[Dir] - Params.YieldSlope < Chunk.Bed[Index])
				{
					Move = FMath::Min(Move, Params.WallSpeed * DeltaTime);
				}

				Outflow[Dir] = Move;
				Total += Move;
			}

			if (Total <= 0.f)
			{
				continue;
			}

			// SCALED TOGETHER RATHER THAN CLAMPED ONE BY ONE. This is the line that
			// makes conservation structural: a cell that would give away more than
			// it holds gives away exactly what it holds, in the proportions it
			// wanted, and no branch anywhere can produce a negative depth.
			const float Scale = (Total > Depth) ? (Depth / Total) : 1.f;

			for (int32 Dir = 0; Dir < 4; ++Dir)
			{
				if (Outflow[Dir] <= 0.f)
				{
					continue;
				}

				const float Move = Outflow[Dir] * Scale;

				FIntPoint NeighbourCoord;
				int32 Unused = 0;
				StepCell(Coord, Index, NeighbourDX[Dir], NeighbourDY[Dir],
					NeighbourCoord, Unused);

				// THE SLOT THE HEAD WAS MEASURED AGAINST, not a fresh step. Working
				// it out twice would be working it out twice differently the first
				// time somebody changed one of them.
				const int32 NeighbourIndex = Slot[Dir];

				AddDelta(Chunk, Index, -Move);

				Chunk.NetFluxX[Index] += Move * NeighbourDX[Dir];
				Chunk.NetFluxY[Index] += Move * NeighbourDY[Dir];

				if (FARPGFluidChunk* Neighbour = FindChunk(NeighbourCoord))
				{
					AddDelta(*Neighbour, NeighbourIndex, Move);
				}
				else
				{
					CrossDeposits.Add({ NeighbourCoord, NeighbourIndex, Move,
						NeighbourBeds[Dir] });
				}
			}
		}
	}

	// --- Chunks that did not exist when the sweep ran ----------------------
	//
	// Folded into the ordinary deltas rather than applied separately, so there is
	// one apply pass and one set of bookkeeping. Fetched fresh per deposit,
	// because adding a chunk can rehash the map and invalidate anything held.

	for (const FARPGFluidCrossDeposit& Deposit : CrossDeposits)
	{
		float Bed = Deposit.Bed;
		const int32 Layer = EnsureLayer(Deposit.Coord, CellOfSlot(Deposit.Index),
			Deposit.Bed, Bed);

		if (Layer == INDEX_NONE)
		{
			continue;
		}

		FARPGFluidChunk& Chunk = Chunks.FindChecked(Deposit.Coord);
		AddDelta(Chunk, SlotOf(Layer, CellOfSlot(Deposit.Index)), Deposit.Amount);
	}

	// --- Apply -------------------------------------------------------------
	//
	// OVER EVERY CHUNK, for the same reason the clear is: a chunk that only
	// RECEIVED this step has a delta and no active cells, and skipping it would
	// subtract the water from the giver and never credit the taker.

	for (TPair<FIntPoint, FARPGFluidChunk>& Pair : Chunks)
	{
		const FIntPoint Coord = Pair.Key;
		FARPGFluidChunk& Chunk = Pair.Value;

		for (int32 Index : Chunk.Touched)
		{
			const float Before = Chunk.Depth[Index];
			const float After = FMath::Max(0.f, Before + Chunk.Delta[Index]);

			Chunk.Depth[Index] = After;

			if (Before <= 0.f && After > 0.f)
			{
				++Chunk.WetCells;

				// THE FLOOR UNDER GROUND THAT HAS JUST BECOME WET. Every cell the
				// sweep can reach was probed as a NEIGHBOUR, but that answer was
				// thrown away with the loop iteration -- so a cell that received
				// water and then became a source next step would compute its head
				// from a bed of zero. On flat ground at height zero that is
				// invisible; on a ramp it means water arrives and then refuses to
				// go any further, which is exactly what it did.
				if (!EnumHasAnyFlags(Chunk.Flags[Index], EARPGFluidCellFlags::BedKnown))
				{
					float Height = 0.f;

					if (BedProbe && BedProbe(CellCentre(Coord, Index), Height))
					{
						Chunk.Bed[Index] = Height;
					}

					Chunk.Flags[Index] |= EARPGFluidCellFlags::BedKnown;
				}
			}
			else if (Before > 0.f && After <= 0.f)
			{
				--Chunk.WetCells;
			}

			// DERIVED, NEVER INTEGRATED. Velocity here is a report of what the
			// fluxes did rather than a state of its own, which is why nothing can
			// make it disagree with where the water actually went.
			const float Carrier = FMath::Max(After, Params.MinimumFilm);
			Chunk.VelX[Index] = Chunk.NetFluxX[Index] * Cell / (DeltaTime * Carrier);
			Chunk.VelY[Index] = Chunk.NetFluxY[Index] * Cell / (DeltaTime * Carrier);
		}
	}

	// --- Rebuild the frontier ----------------------------------------------
	//
	// A CELL IS WORTH VISITING IF IT OR A NEIGHBOUR HAS WATER, which is one ring
	// wider than "is wet" and is what lets a puddle grow into dry ground. Walked
	// over the previous frontier and everything this step touched rather than over
	// the grid, so an idle chunk costs nothing.

	TArray<TPair<FIntPoint, int32>> Frontier;

	for (TPair<FIntPoint, FARPGFluidChunk>& Pair : Chunks)
	{
		for (int32 Index : Pair.Value.Active)
		{
			Frontier.Emplace(Pair.Key, Index);
		}

		for (int32 Index : Pair.Value.Touched)
		{
			Frontier.Emplace(Pair.Key, Index);
		}
	}

	// Cleared before anything is marked, or a stamp left over from this step's
	// list would refuse the same cell a place in the next one.
	for (TPair<FIntPoint, FARPGFluidChunk>& Pair : Chunks)
	{
		FARPGFluidChunk& Chunk = Pair.Value;

		for (int32 Index : Chunk.Active)
		{
			Chunk.ActiveStamp[Index] = 0;
		}

		Chunk.Active.Reset();
		Chunk.NextActive.Reset();
	}

	for (const TPair<FIntPoint, int32>& At : Frontier)
	{
		const FARPGFluidChunk* Chunk = FindChunk(At.Key);

		if (!Chunk || Chunk->Depth[At.Value] <= 0.f)
		{
			continue;
		}

		MarkNextActive(Chunks.FindChecked(At.Key), At.Value);

		for (int32 Dir = 0; Dir < 4; ++Dir)
		{
			FIntPoint NeighbourCoord;
			int32 NeighbourIndex = 0;
			StepCell(At.Key, At.Value, NeighbourDX[Dir], NeighbourDY[Dir],
				NeighbourCoord, NeighbourIndex);

			MarkNextActive(FindOrAddChunk(NeighbourCoord), NeighbourIndex);
		}
	}

	// --- Swap, and let the freeze stamps go --------------------------------

	for (TPair<FIntPoint, FARPGFluidChunk>& Pair : Chunks)
	{
		FARPGFluidChunk& Chunk = Pair.Value;

		Chunk.Active = MoveTemp(Chunk.NextActive);
		Chunk.NextActive.Reset();

		for (int32 Index : Chunk.Active)
		{
			Chunk.ActiveStamp[Index] = 1;
		}

		// CLEARED AT THE END OF THE STEP, so a freeze cannot hold ground for longer
		// than the reaction that took it -- see EARPGFluidCellFlags.
		for (EARPGFluidCellFlags& Flag : Chunk.Flags)
		{
			Flag &= ~EARPGFluidCellFlags::Frozen;
		}
	}
}

double FARPGFluidField::ApplyWeather(float DepthPerSecond, float DeltaTime)
{
	if (FMath::IsNearlyZero(DepthPerSecond) || DeltaTime <= 0.f)
	{
		return 0.0;
	}

	const float Change = DepthPerSecond * DeltaTime;
	const float Cell = GetCellSize();
	const double CellArea = static_cast<double>(Cell) * Cell;

	double Total = 0.0;

	for (TPair<FIntPoint, FARPGFluidChunk>& Pair : Chunks)
	{
		FARPGFluidChunk& Chunk = Pair.Value;

		for (int32 Index = 0; Index < Chunk.Depth.Num(); ++Index)
		{
			const float Before = Chunk.Depth[Index];

			// RAIN FALLS ON DRY GROUND TOO, but not on ground under a slab and not
			// where the field has never been -- a puddle that grew by raining on
			// every cell in every allocated chunk would be a puddle the size of the
			// chunk within a few seconds.
			if (Before <= 0.f)
			{
				continue;
			}

			const float After = FMath::Max(0.f, Before + Change);

			Chunk.Depth[Index] = After;
			Total += static_cast<double>(After - Before) * CellArea;

			if (After <= 0.f)
			{
				--Chunk.WetCells;
			}
			else
			{
				MarkActive(Chunk, Index);
			}
		}
	}

	return Total;
}

// ---------------------------------------------------------------------------
// Regions

void FARPGFluidField::FindRegions(TArray<FRegion>& OutRegions) const
{
	OutRegions.Reset();

	const float Cell = GetCellSize();
	const double CellArea = static_cast<double>(Cell) * Cell;

	// Visited by (chunk, index). A set rather than a per-chunk array because the
	// walk crosses chunk edges freely and the alternative is allocating a full
	// grid of stamps per chunk for a pass that touches only the wet ones.
	TSet<TPair<FIntPoint, int32>> Visited;
	TArray<TPair<FIntPoint, int32>> Pending;

	for (const TPair<FIntPoint, FARPGFluidChunk>& Pair : Chunks)
	{
		if (Pair.Value.WetCells <= 0)
		{
			continue;
		}

		for (int32 Index = 0; Index < Pair.Value.Depth.Num(); ++Index)
		{
			if (Pair.Value.Depth[Index] <= 0.f)
			{
				continue;
			}

			const TPair<FIntPoint, int32> Seed(Pair.Key, Index);

			if (Visited.Contains(Seed))
			{
				continue;
			}

			FRegion Region;
			Region.Bounds.Init();

			// A BODY IS ALL ON ONE FLOOR by construction -- flow only ever connects
			// slots in the same layer -- so the seed's layer is the body's.
			Region.Layer = LayerOfSlot(Index);

			Pending.Reset();
			Pending.Add(Seed);
			Visited.Add(Seed);

			FVector2D Sum = FVector2D::ZeroVector;

			while (Pending.Num() > 0)
			{
				const TPair<FIntPoint, int32> At = Pending.Pop(EAllowShrinking::No);
				const FARPGFluidChunk& Chunk = Chunks.FindChecked(At.Key);

				const FVector2D Centre = CellCentre(At.Key, At.Value);
				const float Depth = Chunk.Depth[At.Value];

				Region.Cells.Add(At);
				Region.Volume += static_cast<double>(Depth) * CellArea;
				Region.Area += CellArea;
				Region.Bounds += Centre;
				Sum += Centre;

				const float Level = Chunk.Bed[At.Value] + Depth;

				if (Region.Cells.Num() == 1)
				{
					Region.MaxLevel = Level;
					Region.MinBed = Chunk.Bed[At.Value];
				}
				else
				{
					Region.MaxLevel = FMath::Max(Region.MaxLevel, Level);
					Region.MinBed = FMath::Min(Region.MinBed, Chunk.Bed[At.Value]);
				}

				for (int32 Dir = 0; Dir < 4; ++Dir)
				{
					FIntPoint NeighbourCoord;
					int32 NeighbourIndex = 0;
					StepCell(At.Key, At.Value, NeighbourDX[Dir], NeighbourDY[Dir],
						NeighbourCoord, NeighbourIndex);

					const FARPGFluidChunk* Neighbour = FindChunk(NeighbourCoord);

					if (!Neighbour)
					{
						continue;
					}

					// THE FLOOR, NOT THE LAYER NUMBER -- the same match the solver
					// makes, and it has to be the same or a body would be defined
					// differently from the way the water moved. StepCell keeps the
					// layer INDEX, which across two cells means nothing; what makes
					// two slots the same surface is that their beds agree.
					const int32 Matched = FindLayerNear(*Neighbour,
						CellOfSlot(NeighbourIndex), Chunk.Bed[At.Value]);

					if (Matched == INDEX_NONE)
					{
						continue;
					}

					const int32 Slot = SlotOf(Matched, CellOfSlot(NeighbourIndex));

					if (Neighbour->Depth[Slot] <= 0.f)
					{
						continue;
					}

					const TPair<FIntPoint, int32> Next(NeighbourCoord, Slot);

					if (Visited.Contains(Next))
					{
						continue;
					}

					Visited.Add(Next);
					Pending.Add(Next);
				}
			}

			Region.Centroid = Sum / Region.Cells.Num();

			Region.Lookup.Append(Region.Cells);

			// The bounds are built from cell CENTRES, so a one-cell body would have
			// none at all. Grown by a half cell so a region's box contains the
			// ground it actually covers.
			Region.Bounds = Region.Bounds.ExpandBy(Cell * 0.5f);

			OutRegions.Add(MoveTemp(Region));
		}
	}
}

int32 FARPGFluidField::FindRegionAt(const TArray<FRegion>& Regions, const FVector2D& World) const
{
	FIntPoint Coord;
	int32 Index = 0;
	ResolveCell(World, Coord, Index);

	const TPair<FIntPoint, int32> At(Coord, Index);

	for (int32 Which = 0; Which < Regions.Num(); ++Which)
	{
		if (Regions[Which].Lookup.Contains(At))
		{
			return Which;
		}
	}

	return INDEX_NONE;
}

void FARPGFluidField::BuildRegionOutline(const FRegion& Region, TArray<FVector2D>& OutRing) const
{
	OutRing.Reset();

	if (Region.Cells.Num() == 0)
	{
		return;
	}

	const float Cell = GetCellSize();

	// EVERY EDGE WITH THE BODY ON ITS LEFT. Walked in that one direction, the
	// boundary segments of any 4-connected set join end to end into closed loops
	// with no further decisions to make -- which is the whole reason to emit
	// directed edges rather than undirected ones and sort it out afterwards.
	//
	// Corners are indexed in global cell coordinates, so corner (i, j) is the
	// world point (i * Cell, j * Cell) and two cells sharing an edge name the same
	// two corners for it. Integers, so the join is exact rather than within a
	// tolerance.
	TMap<FIntPoint, FIntPoint> Edges;
	Edges.Reserve(Region.Cells.Num() * 2);

	for (const TPair<FIntPoint, int32>& At : Region.Cells)
	{
		const int32 GlobalX = At.Key.X * Resolution + (At.Value % Resolution);
		const int32 GlobalY = At.Key.Y * Resolution + (At.Value / Resolution);

		for (int32 Dir = 0; Dir < 4; ++Dir)
		{
			FIntPoint NeighbourCoord;
			int32 NeighbourIndex = 0;
			ResolveGlobal(GlobalX + NeighbourDX[Dir], GlobalY + NeighbourDY[Dir],
				NeighbourCoord, NeighbourIndex);

			// SAME FLOOR OR NOTHING. The outline of a balcony puddle must not be
			// closed by the fact that there is water on the ground below it.
			const FARPGFluidChunk* Neighbour = FindChunk(NeighbourCoord);
			const int32 Matched = Neighbour
				? FindLayerNear(*Neighbour, CellOfSlot(NeighbourIndex),
					Chunks.FindChecked(At.Key).Bed[At.Value])
				: INDEX_NONE;

			if (Matched != INDEX_NONE && Region.Lookup.Contains(
					TPair<FIntPoint, int32>(NeighbourCoord,
						SlotOf(Matched, CellOfSlot(NeighbourIndex)))))
			{
				continue;
			}

			// East, west, north, south -- matching NeighbourDX/DY -- each emitted
			// so that the cell is on the left of the direction of travel.
			switch (Dir)
			{
			case 0:
				Edges.Add(FIntPoint(GlobalX + 1, GlobalY), FIntPoint(GlobalX + 1, GlobalY + 1));
				break;
			case 1:
				Edges.Add(FIntPoint(GlobalX, GlobalY + 1), FIntPoint(GlobalX, GlobalY));
				break;
			case 2:
				Edges.Add(FIntPoint(GlobalX + 1, GlobalY + 1), FIntPoint(GlobalX, GlobalY + 1));
				break;
			default:
				Edges.Add(FIntPoint(GlobalX, GlobalY), FIntPoint(GlobalX + 1, GlobalY));
				break;
			}
		}
	}

	// Walked into loops, consuming each edge once. A region with a blocked cell in
	// the middle produces an inner loop as well as an outer one; the largest is
	// the answer -- see the header on why a fluid keeps no hole.
	TArray<FVector2D> Best;
	double BestArea = 0.0;

	while (Edges.Num() > 0)
	{
		const FIntPoint Start = TMap<FIntPoint, FIntPoint>::TConstIterator(Edges).Key();

		TArray<FVector2D> Loop;
		FIntPoint At = Start;

		// Bounded by the edge count, so a malformed set cannot spin forever.
		for (int32 Guard = Edges.Num() + 1; Guard > 0; --Guard)
		{
			const FIntPoint* Next = Edges.Find(At);

			if (!Next)
			{
				break;
			}

			const FIntPoint To = *Next;
			Edges.Remove(At);

			Loop.Add(FVector2D(At.X * Cell, At.Y * Cell));
			At = To;

			if (At == Start)
			{
				break;
			}
		}

		const double Area = ARPGFluidGeometry::PolygonArea(Loop);

		if (Loop.Num() >= 3 && Area > BestArea)
		{
			BestArea = Area;
			Best = MoveTemp(Loop);
		}
	}

	if (Best.Num() < 3)
	{
		return;
	}

	// COLLINEAR RUNS COLLAPSED. Traced edge by edge, a two-metre straight bank is
	// five vertices saying the same thing, and everything downstream -- the
	// booleans, the triangulator, the distance queries -- pays per vertex.
	OutRing.Reserve(Best.Num());

	for (int32 Index = 0; Index < Best.Num(); ++Index)
	{
		const FVector2D& Previous = Best[(Index + Best.Num() - 1) % Best.Num()];
		const FVector2D& Here = Best[Index];
		const FVector2D& Next = Best[(Index + 1) % Best.Num()];

		const FVector2D In = Here - Previous;
		const FVector2D Out = Next - Here;

		if (!FMath::IsNearlyZero(In.X * Out.Y - In.Y * Out.X))
		{
			OutRing.Add(Here);
		}
	}

	if (OutRing.Num() < 3)
	{
		OutRing = MoveTemp(Best);
	}
}

// ---------------------------------------------------------------------------
// One body's cells
//
// See the header on why a body is addressed by the slots it holds rather than
// by a layer number or a height.

double FARPGFluidField::GetVolumeOfCells(const FCellSet& InCells) const
{
	const float Cell = GetCellSize();
	const double CellArea = static_cast<double>(Cell) * Cell;

	double Total = 0.0;

	for (const TPair<FIntPoint, int32>& At : InCells)
	{
		if (const FARPGFluidChunk* Chunk = FindChunk(At.Key))
		{
			Total += static_cast<double>(Chunk->Depth[At.Value]) * CellArea;
		}
	}

	return Total;
}

double FARPGFluidField::GetAreaOfCells(const FCellSet& InCells) const
{
	const float Cell = GetCellSize();
	const double CellArea = static_cast<double>(Cell) * Cell;

	double Total = 0.0;

	for (const TPair<FIntPoint, int32>& At : InCells)
	{
		const FARPGFluidChunk* Chunk = FindChunk(At.Key);

		if (Chunk && Chunk->Depth[At.Value] > 0.f)
		{
			Total += CellArea;
		}
	}

	return Total;
}

double FARPGFluidField::RemoveFromCells(const FCellSet& InCells, const FVector2D& Near,
	double Volume)
{
	if (Volume <= 0.0)
	{
		return 0.0;
	}

	const float Cell = GetCellSize();
	const double CellArea = static_cast<double>(Cell) * Cell;

	struct FCandidate
	{
		FIntPoint Coord;
		int32 Slot;
		float DistanceSq;
	};

	TArray<FCandidate> Candidates;
	Candidates.Reserve(InCells.Num());

	for (const TPair<FIntPoint, int32>& At : InCells)
	{
		const FARPGFluidChunk* Chunk = FindChunk(At.Key);

		if (!Chunk || Chunk->Depth[At.Value] <= 0.f)
		{
			continue;
		}

		Candidates.Add({ At.Key, At.Value,
			static_cast<float>(FVector2D::DistSquared(CellCentre(At.Key, At.Value), Near)) });
	}

	Candidates.Sort([](const FCandidate& A, const FCandidate& B)
		{
			return A.DistanceSq < B.DistanceSq;
		});

	double Remaining = Volume;

	for (const FCandidate& Candidate : Candidates)
	{
		if (Remaining <= 0.0)
		{
			break;
		}

		FARPGFluidChunk& Chunk = Chunks.FindChecked(Candidate.Coord);

		const double Available = static_cast<double>(Chunk.Depth[Candidate.Slot]) * CellArea;
		const double Taken = FMath::Min(Available, Remaining);

		Chunk.Depth[Candidate.Slot] -= static_cast<float>(Taken / CellArea);
		Remaining -= Taken;

		if (Chunk.Depth[Candidate.Slot] <= 0.f)
		{
			Chunk.Depth[Candidate.Slot] = 0.f;
			--Chunk.WetCells;
		}

		WakeAround(Candidate.Coord, Candidate.Slot);
	}

	return Volume - Remaining;
}

double FARPGFluidField::RemoveAreaFromCells(const FCellSet& InCells, const FVector2D& Near,
	double Area)
{
	if (Area <= 0.0)
	{
		return 0.0;
	}

	const float Cell = GetCellSize();
	const double CellArea = static_cast<double>(Cell) * Cell;

	struct FCandidate
	{
		FIntPoint Coord;
		int32 Slot;
		float DistanceSq;
	};

	TArray<FCandidate> Candidates;
	Candidates.Reserve(InCells.Num());

	for (const TPair<FIntPoint, int32>& At : InCells)
	{
		const FARPGFluidChunk* Chunk = FindChunk(At.Key);

		if (!Chunk || Chunk->Depth[At.Value] <= 0.f)
		{
			continue;
		}

		Candidates.Add({ At.Key, At.Value,
			static_cast<float>(FVector2D::DistSquared(CellCentre(At.Key, At.Value), Near)) });
	}

	Candidates.Sort([](const FCandidate& A, const FCandidate& B)
		{
			return A.DistanceSq < B.DistanceSq;
		});

	double Taken = 0.0;

	for (const FCandidate& Candidate : Candidates)
	{
		if (Taken >= Area)
		{
			break;
		}

		FARPGFluidChunk& Chunk = Chunks.FindChecked(Candidate.Coord);

		// WHOLE CELLS. Half-emptying the last one would leave a film that counts
		// as wet ground and put the area back where it started.
		Chunk.Depth[Candidate.Slot] = 0.f;
		--Chunk.WetCells;
		Taken += CellArea;

		WakeAround(Candidate.Coord, Candidate.Slot);
	}

	return Taken;
}

double FARPGFluidField::ConsumeCellsIn(const FCellSet& InCells, const TArray<FVector2D>& Region)
{
	if (Region.Num() < 3)
	{
		return 0.0;
	}

	const float Cell = GetCellSize();
	const double CellArea = static_cast<double>(Cell) * Cell;

	double Taken = 0.0;

	for (const TPair<FIntPoint, int32>& At : InCells)
	{
		FARPGFluidChunk* Chunk = FindChunk(At.Key);

		if (!Chunk)
		{
			continue;
		}

		// COVERED, NOT CONTAINED -- see ConsumeRegion for why the half cell is
		// there, and what an inscribed agent circle leaves behind without it.
		if (ARPGFluidGeometry::PolygonSignedDistance(Region, CellCentre(At.Key, At.Value))
			> Cell * 0.5)
		{
			continue;
		}

		Chunk->Flags[At.Value] |= EARPGFluidCellFlags::Frozen;

		if (Chunk->Depth[At.Value] <= 0.f)
		{
			continue;
		}

		Taken += static_cast<double>(Chunk->Depth[At.Value]) * CellArea;
		Chunk->Depth[At.Value] = 0.f;
		--Chunk->WetCells;

		WakeAround(At.Key, At.Value);
	}

	return Taken;
}

// ---------------------------------------------------------------------------
// Handing the field to the GPU

void FARPGFluidField::SampleWindow(const FWindow& Window, TArray<FFloat16Color>& OutPixels,
	const FCellSet* Only) const
{
	const int32 Side = FMath::Max(1, Window.Resolution);
	const int32 Count = Side * Side;

	OutPixels.SetNumUninitialized(Count);

	if (Window.Extent <= 0.f)
	{
		FMemory::Memzero(OutPixels.GetData(), Count * sizeof(FFloat16Color));
		return;
	}

	const float Step = Window.Extent / Side;

	// WHICH TEXELS THE FIELD ACTUALLY HAS AN OPINION ABOUT. A byte rather than a
	// bit array so the parallel fill below can write entries without two threads
	// sharing a word.
	TArray<uint8> Known;
	Known.SetNumZeroed(Count);

	// A ROW PER TASK. Every texel is an independent read of a chunk nobody is
	// writing while this runs, so the only reason this was serial is that it was
	// written before anyone measured it: 256 texels square costs 1.38ms on one
	// core, four times a second per element, on the game thread.
	//
	// ROWS RATHER THAN TEXELS, because the work per texel is a hash lookup and a
	// few reads -- small enough that per-item task overhead would cost more than
	// the work. A row is Side texels of it.
	ParallelFor(Side, [this, &Window, &OutPixels, &Known, Only, Side, Step](int32 Y)
		{
		for (int32 X = 0; X < Side; ++X)
		{
			// Texel CENTRES, so the picture and the thing it is a picture of agree
			// about where a texel is. Sampling the corner puts the whole image half
			// a texel off, which reads as the water being offset from the ground.
			const FVector2D At(
				Window.Origin.X + (X + 0.5f) * Step,
				Window.Origin.Y + (Y + 0.5f) * Step);

			FFloat16Color& Pixel = OutPixels[Y * Side + X];

			FIntPoint Coord;
			int32 Cell = 0;
			ResolveCell(At, Coord, Cell);

			const FARPGFluidChunk* Chunk = FindChunk(Coord);

			if (!Chunk)
			{
				// NEVER VISITED IS NOT THE SAME AS DRY, but for a picture it is: the
				// field has no opinion about ground it has never had water on, and
				// the material has nothing to draw there either way.
				Pixel = FFloat16Color(FLinearColor(0.f, Window.NearZ, Window.NearZ, 0.f));
				continue;
			}

			// THE VIEWER'S OWN FLOOR -- see FWindow::NearZ. Falling back to whichever
			// layer holds water keeps a puddle visible where the probe has not been
			// asked about the viewer's height yet.
			int32 Layer = FindLayerNear(*Chunk, Cell, Window.NearZ);

			if (Layer == INDEX_NONE)
			{
				Layer = FindWetLayer(*Chunk, Cell);
			}

			if (Layer == INDEX_NONE)
			{
				Pixel = FFloat16Color(FLinearColor(0.f, Window.NearZ, Window.NearZ, 0.f));
				continue;
			}

			const int32 Slot = SlotOf(Layer, Cell);

			Known[Y * Side + X] = 1;

			const float Bed = Chunk->Bed[Slot];

			// INTERPOLATED, NOT THE CELL'S OWN. The picture is usually FINER than
			// the field -- a couple of centimetres a texel against forty -- so
			// writing each cell's raw depth would stamp forty-centimetre squares
			// into the texture, and no amount of filtering afterwards can take out
			// blocks that are already several texels across. Reading the field the
			// way every other consumer reads it gives a smooth surface sampled at
			// the resolution the material can afford.
			//
			// The categorical channels below stay nearest, because a cell either
			// has a wall in it or does not and there is nothing between.
			const float Depth = SampleDepth(At, Layer);

			const bool bBlocked =
				EnumHasAnyFlags(Chunk->Flags[Slot], EARPGFluidCellFlags::Blocked);
			// max(floor, solid) -- see the header. To a shallow-water solver a wall is
			// floor that is higher up, and saying it that way means nothing on the
			// GPU has to know what a wall is.
			//
			// BLOCKED, NOT ROOFED, and the difference is a floe. A grounded slab is
			// bottom that has risen and water has to go around it. A FLOATING one has
			// water underneath -- that is what makes it a floe rather than a plug,
			// and the field's own solver already lets water run beneath it. Raising
			// the contour to a floating slab's top would tell the sheet the riverbed
			// was up there, and it would draw water sitting ON the ice.
			//
			// So a floe contributes nothing here. It is drawn by its own mesh, and
			// the water under it is drawn at the depth the field says it has.
			const float Bottom = bBlocked ? FMath::Max(Bed, Chunk->SolidTop[Slot]) : Bed;

			// A IS BLOCKED AND NOTHING ELSE, and it took a shader to notice why.
			//
			// This channel used to pack two bits -- 1 blocked, 2 roofed -- which is
			// tidy and unusable. A FLOATING FLOE IS ROOFED AND NOT BLOCKED, so it
			// read as 2, and the obvious test on the far end (`A >= 1`) turned
			// every floe into a dam. That is exactly the distinction the two flags
			// exist to keep apart, undone by packing them into one number.
			//
			// AND THE TEXTURE IS FILTERED. Even with the right bit test, bilinear
			// interpolation between a 1 and a 2 gives 1.5 somewhere along every
			// boundary, and a bit test on a value that was never meant to be
			// averaged is a coin toss at the edge of every wall.
			//
			// Roofed is not lost: B carries the top of whatever stands here and G
			// carries the floor, so `B > G` is the same question. Blocked cannot be
			// derived that way -- a floe raises B without blocking anything -- which
			// is why it is the one that gets the channel.
			const float Flags = bBlocked ? 1.f : 0.f;

			// A BLOCKED TEXEL IS DRY, whatever the interpolation says. Reading the
			// depth smoothly is right everywhere else and wrong here: the four
			// cells around a wall are wet, so a bilinear read inside the wall comes
			// back with water in it and the surface laps a few centimetres into the
			// stone. The material could be made to work it out from the bottom
			// contour instead, but a channel that means "the depth here" should not
			// need a second channel to be believed.
			// SOMEBODY ELSE'S WATER IS NOT THIS SHEET'S TO DRAW -- see the header.
			// The floor and the collider flag still go in, because the sheet lies
			// on the one and parts around the other whoever owns the water.
			const bool bMine = !Only || Only->Contains(TPair<FIntPoint, int32>(Coord, Cell));

			Pixel = FFloat16Color(FLinearColor(
				(bBlocked || !bMine) ? 0.f : Depth, Bed, Bottom, Flags));
		}
		});

	// --- Giving the unknown ground a plausible floor --------------------------
	//
	// A TEXEL THE FIELD KNOWS NOTHING ABOUT STILL GETS SAMPLED. The material reads
	// this picture BILINEARLY, so every texel on the dry side of a puddle's rim is
	// blended into the wet one next to it -- and the fallback above reports the
	// VIEWER'S Z as the floor there. Measured on a real puddle: 180 wet/dry pairs
	// along one rim with floors three metres apart. The surface is drawn at floor
	// plus depth, so the whole boundary got heights halfway to the player's chest
	// and tore itself apart. It looked better as you walked towards it, because
	// the number being blended in was your own height.
	//
	// ONE TEXEL OF DILATION IS ENOUGH, because that is exactly how far a bilinear
	// tap reaches. Depth is left alone -- these cells stay dry and stay masked out
	// -- so this changes nothing about what is drawn, only about what the filter
	// finds when it reaches across the edge.
	//
	// FROM A SNAPSHOT, not in place, or the fill would march outward across the
	// whole window one texel per row instead of stopping at one.
	const TArray<FFloat16Color> Sampled = OutPixels;

	ParallelFor(Side, [&Sampled, &OutPixels, &Known, Side](int32 Y)
		{
		for (int32 X = 0; X < Side; ++X)
		{
			const int32 Index = Y * Side + X;

			if (Known[Index])
			{
				continue;
			}

			const int32 Neighbours[4] =
			{
				X > 0 ? Index - 1 : INDEX_NONE,
				X + 1 < Side ? Index + 1 : INDEX_NONE,
				Y > 0 ? Index - Side : INDEX_NONE,
				Y + 1 < Side ? Index + Side : INDEX_NONE,
			};

			for (const int32 Neighbour : Neighbours)
			{
				if (Neighbour == INDEX_NONE || !Known[Neighbour])
				{
					continue;
				}

				// THE NEIGHBOUR'S FLOOR INTO BOTH CHANNELS, and deliberately not its
				// bottom contour. Borrow B from a texel that happens to be inside a
				// wall and this cell inherits a raised contour without being blocked
				// -- which fattens every solid by one texel in the sheet's eyes and
				// breaks the invariant that a raised contour means something is
				// standing there. The floor is the honest thing to borrow.
				OutPixels[Index].G = Sampled[Neighbour].G;
				OutPixels[Index].B = Sampled[Neighbour].G;
				break;
			}
		}
		});
}
