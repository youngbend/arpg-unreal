// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Math/Float16Color.h"

/**
 * A body of fluid as a HEIGHTFIELD rather than an outline.
 *
 * WHAT THIS REPLACES AND WHY. A pool used to be a polygon, and that bought a
 * great deal: no quantisation, exact boolean answers, and an outline so small it
 * could be replicated. What it could not buy is the two things the elemental
 * work now needs. It has no FLOW -- the whole simulation was one inward or
 * outward offset four times a second -- and it cannot go AROUND anything,
 * because a triangulated pool carries at most one hole and a puddle with two
 * slabs standing in it has two.
 *
 * A GRID HAS BOTH FOR FREE, and that is the entire argument. Water routes around
 * a solid because the solid's cells are marked and the solver does not write
 * into them; there is no topology, no boolean, and no limit on how many
 * obstacles a puddle may have. Head differences between neighbours are what
 * makes it run downhill, which is a property the polygon model could not express
 * at all.
 *
 * THE GRID WAS ALREADY TRIED AND IT FAILED, which is worth saying plainly. See
 * the deleted ARPGSolidField: a heightfield of int16 millimetres, REPLICATED,
 * carrying a runoff film. It died of quantisation and wire cost. Neither applies
 * here -- this is floats, it is not replicated at all, and it is not the shape of
 * anything a client has to agree with byte for byte. What survived that design
 * is its solver, and the five properties on UARPGFluidDefinition that have been
 * dead ever since are the knobs this one reads.
 *
 * NOT A UOBJECT, DELIBERATELY. UARPGSpreadSubsystem keeps its chunks private and
 * its tests can therefore only reach the field through a UWorld, which makes
 * every assertion about the solver an integration test. This is a plain struct:
 * a test constructs one, gives it a floor as a lambda, pours into it, steps it,
 * and asserts. No world, no actors, no renderer, and it runs under -nullrhi like
 * everything else.
 *
 * ONE FIELD PER ELEMENT. Water and lava are two of these, not two slots in one,
 * because a cell holding both is precisely the thing the reaction solver exists
 * to resolve and not a state the solver should be able to represent quietly.
 */

/**
 * The fluid's own character, copied out of its definition.
 *
 * VALUES RATHER THAN A POINTER so the field has no UObject reference of any
 * kind. That is what lets a test build one from four literals instead of
 * standing up a data asset, and it is the only reason this class can be unit
 * tested rather than only played.
 */
struct ARPGWORLD_API FARPGFluidFieldParams
{
	/**
	 * Fraction of the available head moved per second.
	 *
	 * NOT A SPEED. The solver moves a share of the height difference between
	 * neighbouring cells, so this is a rate of SETTLING: high is a thin quick
	 * sheet, low is a slow ooze. Straight off UARPGFluidDefinition::FlowRate,
	 * which has documented exactly this since the runoff film and has been read
	 * by nothing since that film was deleted.
	 */
	float FlowRate = 6.f;

	/**
	 * Head, in cm, below which this fluid does not move between two cells.
	 *
	 * THE HALF OF VISCOSITY A RATE CANNOT EXPRESS. A slower rate makes a fluid
	 * arrive later; a yield slope makes it STOP. It is also what makes a viscous
	 * fluid stand thick rather than spreading, which falls out rather than being
	 * written: a fluid that needs more head before it moves necessarily piles up
	 * until it has that head.
	 */
	float YieldSlope = 0.f;

	/**
	 * Below this depth in cm a cell is too thin to flow.
	 *
	 * A FLOOR ON MOVEMENT, NOT ON EXISTENCE, and that is a deliberate departure
	 * from what the property's own comment describes. Deleting sub-film water was
	 * the old film's answer to an asymptote -- each step moves a share of what is
	 * left, so depth approaches zero and never arrives -- and it makes the solver
	 * lose mass, which is the one property a fluid simulation cannot be allowed
	 * to get wrong and the one this is most easily tested for. Refusing to MOVE
	 * below the film ends the asymptote just as well: the film stops, exactly, in
	 * finite steps, and the water is still all there. What removes it is
	 * evaporation, which is a weather term and is supposed to lose mass.
	 */
	float MinimumFilm = 0.05f;

	/**
	 * How fast this runs down a vertical face, in cm per second.
	 *
	 * THE ONE PLACE VISCOSITY IS A SPEED. FlowRate is a share of the head because
	 * across a heightfield the slope is what drives the flow; a wall has no slope
	 * to speak of, gravity is the whole of the forcing, and what distinguishes
	 * water from lava there is simply how fast the stuff moves. Water is metres a
	 * second, lava is a crawl -- and a crawl is most of what tells you which one
	 * you are looking at.
	 */
	float WallSpeed = 200.f;

	/** How deep a fresh deposit is laid, in cm. Volume divided by this is area. */
	float DepositDepth = 20.f;

	/**
	 * How far below where fluid is being laid the floor may be and still take it.
	 *
	 * A DEPOSIT IS NOT A FALL. A spell finishing on the lip of a platform wets the
	 * platform; the ground ten metres down over the edge is somewhere the water
	 * would have to fall to get to, and it gets there by FLOWING off the lip over
	 * the following seconds -- at the fluid's own WallSpeed, which is what makes
	 * lava creep off a wall where water sheets. Laying it there instantly would
	 * skip the part that reads as liquid.
	 */
	float MaxDepositDrop = 60.f;

	/**
	 * How far apart two floors have to be before they are different floors.
	 *
	 * THE ONE NUMBER THE WHOLE LAYER MODEL RESTS ON. Beds within this of each
	 * other are the same surface seen from two cells -- which is what a ramp is,
	 * and why a puddle running down one stays a single body however far it falls.
	 * Beds further apart than this are a balcony and the ground under it, and the
	 * water on them has nothing to do with each other.
	 *
	 * Generous, because the failure modes are asymmetric: too small splits a steep
	 * ramp into unrelated puddles, which is visible and wrong; too large merges a
	 * mezzanine with the floor below, which needs a mezzanine that low to notice.
	 */
	float LayerSeparation = 150.f;
};

/**
 * What is true of one cell besides how much is in it.
 *
 * A BITFIELD RATHER THAN THREE ARRAYS because these are read together and
 * written rarely, which is the opposite of the depth and velocity arrays beside
 * them -- see the chunk's own comment on why those are parallel and flat.
 */
enum class EARPGFluidCellFlags : uint8
{
	None = 0,

	/** The floor height here has actually been traced for, rather than guessed. */
	BedKnown = 1 << 0,

	/**
	 * A solid stands here and fluid may not enter.
	 *
	 * SET ONLY WHERE THE SOLID REACHES THE FLOOR. A floe FLOATS, and water genuinely
	 * runs underneath one -- that is what makes it a floe rather than a plug -- so a
	 * slab whose underside is clear of the bed marks SolidTop and nothing else.
	 */
	Blocked = 1 << 1,

	/**
	 * A solid stands here, whether or not it reaches the floor.
	 *
	 * THE OTHER QUESTION, and keeping it apart from Blocked is not pedantry. A
	 * floe ROOFS the water it floats on -- a bolt that struck the ice must not
	 * enter the water underneath and conduct from there -- while blocking nothing,
	 * because water genuinely runs under one. Conflating the two would either make
	 * floes dam the sea or make lightning ignore them.
	 */
	Roofed = 1 << 3,

	/**
	 * There is no floor here at all.
	 *
	 * NOT THE SAME AS A LOW ONE. A cliff is ground that falls away and water pours
	 * down it; this is a hole in the world -- the edge of a platform with nothing
	 * modelled underneath -- and fluid that ran into it would leave the simulation
	 * and take its mass with it. Kept apart from Blocked because ClearSolids wipes
	 * that one every pass and the absence of a floor is not something a slab did.
	 */
	NoFloor = 1 << 4,

	/**
	 * Taken by something this step and not to be refilled before the step ends.
	 *
	 * WHAT FREEZING NEEDS. Water flows back over a gap cut in it, which is right
	 * and is the whole reason a pool never kept a hole -- but not in the instant
	 * the ice forms, where it would refill the cells the floe is being built out
	 * of and the freeze would take nothing. Cleared at the end of every step, so
	 * it cannot outlive the reaction that set it.
	 */
	Frozen = 1 << 2,
};
ENUM_CLASS_FLAGS(EARPGFluidCellFlags)

/**
 * One chunk's grids.
 *
 * PARALLEL FLAT ARRAYS, NOT AN ARRAY OF PER-CELL STRUCTS, for exactly the reason
 * UARPGSpreadSubsystem's FFieldChunk gives: a solver pass touches one field at a
 * time across many cells, so this is the layout the access pattern wants and an
 * array of structs would drag four unread floats through the cache per cell.
 */
struct ARPGWORLD_API FARPGFluidChunk
{
	// EVERY ARRAY HERE IS INDEXED BY SLOT, NOT BY CELL, and a slot is a cell and
	// a LAYER: Layer * CellCount + Cell. See FARPGFluidField's note on why a
	// heightfield needs more than one depth per XY at all.
	//
	// Flat rather than nested, for the same reason they are parallel rather than
	// an array of structs: a solver pass walks one field across many slots, and
	// the layer is just more of the same axis as far as the access pattern is
	// concerned.

	/** Depth of fluid above the bed, in cm. */
	TArray<float> Depth;

	/** Depth-averaged flow, in cm/s. Derived from the fluxes, never integrated. */
	TArray<float> VelX;
	TArray<float> VelY;

	/** World Z of the floor. Meaningless until BedKnown is set. */
	TArray<float> Bed;

	/** World Z of the top of any solid standing here. */
	TArray<float> SolidTop;

	TArray<EARPGFluidCellFlags> Flags;

	/** Cells worth visiting. Rebuilt each step from what stayed wet. */
	TArray<int32> Active;
	TArray<int32> NextActive;
	TArray<uint8> ActiveStamp;

	/**
	 * The step's accumulators, applied only once every chunk has been swept.
	 *
	 * PER CHUNK AND NOT ONE SHARED SET, which is the difference between a solver
	 * that is order-independent and one that is not. A chunk reads its
	 * neighbours' DEPTHS while computing its own transfers, so applying chunk A
	 * before stepping chunk B would let B see water A had already moved and the
	 * answer would depend on which order a TMap happened to iterate in. Keeping
	 * the deltas apart makes the whole sweep a Jacobi update, which is also
	 * exactly the property that will make it a ParallelFor when a profile asks.
	 */
	TArray<float> Delta;

	/** Net depth moved east and north, for deriving velocity after the fact. */
	TArray<float> NetFluxX;
	TArray<float> NetFluxY;

	/** Which cells the pass actually wrote, so clearing costs what was touched. */
	TArray<int32> Touched;
	TArray<uint8> TouchStamp;

	/** So an empty chunk can be freed without walking it. */
	int32 WetCells = 0;

	void Init(int32 SlotCount);
	bool IsAllocated() const { return Depth.Num() > 0; }
};

/**
 * A transfer that crossed a chunk edge, applied after the pass.
 *
 * The same device the spread solver uses and for the same reason: a chunk step
 * writes only its own cells, so the sweep over chunks can run in parallel, and
 * anything landing outside is posted here for the caller to apply once.
 */
struct FARPGFluidCrossDeposit
{
	FIntPoint Coord = FIntPoint::ZeroValue;
	int32 Index = 0;
	float Amount = 0.f;

	/**
	 * The floor the sweep found there.
	 *
	 * CARRIED, because the chunk did not exist when the flux was worked out and
	 * allocating one mid-sweep would rehash the map the sweep is walking. The
	 * apply pass makes the chunk and needs to put the water on the same floor the
	 * head was measured against, not on whichever layer happens to be free.
	 */
	float Bed = 0.f;
};

/**
 * One element's fluid, everywhere in the world.
 *
 * See the file comment for why this exists and why it is not a UObject.
 */
class ARPGWORLD_API FARPGFluidField
{
public:
	/**
	 * How high the floor is at a world XY, and whether there is one at all.
	 *
	 * INJECTED RATHER THAN TRACED, which is what makes the whole class testable.
	 * In the game this is the fluid subsystem's FindGroundAt, with all the same
	 * exclusions -- a puddle must not find itself, or the floe beside it, and
	 * call that the ground. In a test it is two lines that describe a ramp.
	 */
	using FBedProbe = TFunction<bool(const FVector2D& World, float& OutHeight)>;

	FARPGFluidField() = default;

	/** Cell edge, in cm. */
	float GetCellSize() const { return ChunkSize / FMath::Max(1, Resolution); }

	float GetChunkSize() const { return ChunkSize; }
	int32 GetResolution() const { return Resolution; }
	int32 GetLayers() const { return Layers; }

	/** Cells in one layer of one chunk. */
	int32 GetCellCount() const { return Resolution * Resolution; }

	/** Where a (layer, cell) pair lives in the chunk's flat arrays. */
	int32 SlotOf(int32 Layer, int32 Cell) const { return Layer * GetCellCount() + Cell; }

	int32 LayerOfSlot(int32 Slot) const { return Slot / GetCellCount(); }
	int32 CellOfSlot(int32 Slot) const { return Slot % GetCellCount(); }

	/**
	 * Sets the grid up. Safe to call again only while the field is empty.
	 *
	 * Resizing a field with water in it would reinterpret every cell index, so
	 * this asserts rather than quietly scrambling a world's worth of puddles.
	 */
	void Configure(float InChunkSize, int32 InResolution, const FARPGFluidFieldParams& InParams,
		int32 InLayers = 2);

	void SetParams(const FARPGFluidFieldParams& InParams) { Params = InParams; }
	const FARPGFluidFieldParams& GetParams() const { return Params; }

	void SetBedProbe(FBedProbe InProbe) { BedProbe = MoveTemp(InProbe); }

	/** Throws everything away. */
	void Reset();

	// --- Putting fluid in and taking it out ------------------------------------

	/**
	 * Spreads a volume over a disc, laying it at the fluid's own deposit depth.
	 *
	 * A VOLUME AND NOT A RADIUS, because that is what every caller actually has:
	 * a melting slab knows how much rock went, and how wide the puddle should be
	 * is the fluid's business. The radius argument only bounds where it is
	 * allowed to land, so a teaspoon does not paint a ten-metre film one molecule
	 * thick.
	 *
	 * @return the volume actually placed, which is less than asked for when the
	 *         disc is mostly wall.
	 */
	double PourVolume(const FVector2D& Centre, float NearZ, double Volume, float MaxRadius);

	/** Takes a volume off wherever there is any, nearest the centre first. */
	double RemoveVolume(const FVector2D& Centre, float MaxRadius, double Volume,
		int32 Layer = INDEX_NONE);

	/**
	 * Empties every cell inside a polygon and stamps it for the rest of the step.
	 *
	 * WHAT FREEZING DOES, and it is the whole of "no holes": the ice takes exactly
	 * the cells it covers, and the surrounding water does not flow back over them
	 * this step. Compare AARPGSurfaceBody::ConsumeSurfaceRegion, which had to
	 * choose between keeping a hole it could not draw and shrinking a pool
	 * somewhere it was not touched.
	 *
	 * @return the volume removed.
	 */
	double ConsumeRegion(const TArray<FVector2D>& Region, int32 Layer = INDEX_NONE);

	// --- Solids ---------------------------------------------------------------

	/**
	 * Says a solid occupies this cell down to Bottom and up to Top.
	 *
	 * Blocked only where Bottom reaches the floor -- see EARPGFluidCellFlags. A
	 * floe leaves the water under it alone.
	 *
	 * @return the volume of fluid the solid DISPLACED, which the caller has to
	 *         put somewhere. Returned rather than quietly deleted, and rather
	 *         than pushed into the neighbours here: a slab is rasterised cell by
	 *         cell, so pushing outward would shove water into ground the next
	 *         cell of the same slab is about to block, and round the rim of a
	 *         wall that is a puddle's worth of water moved twice.
	 */
	double MarkSolid(const FVector2D& World, float NearZ, float Bottom, float Top);

	/** Forgets every solid mark, before they are all written again. */
	void ClearSolids();

	bool IsBlockedAt(const FVector2D& World, int32 Layer = INDEX_NONE) const;

	/** Is anything solid standing over this point at all, floating or not? */
	bool IsRoofedAt(const FVector2D& World, int32 Layer = INDEX_NONE) const;

	/** World Z of the top of whatever stands here. Zero where nothing does. */
	float SampleSolidTop(const FVector2D& World, int32 Layer = INDEX_NONE) const;

	// --- Asking it things -------------------------------------------------------

	/** Depth in cm, bilinear between cell centres. Zero off the field. */
	float SampleDepth(const FVector2D& World, int32 Layer = INDEX_NONE) const;

	/** Floor height. Falls back to the probe where nothing has been sampled. */
	float SampleBed(const FVector2D& World, int32 Layer = INDEX_NONE) const;

	/** Bed plus depth -- the height of the surface you would swim on. */
	float SampleLevel(const FVector2D& World, int32 Layer = INDEX_NONE) const;

	/** Depth-averaged flow in cm/s. The first real answer GetSurfaceFlowAt has had. */
	FVector2D SampleVelocity(const FVector2D& World, int32 Layer = INDEX_NONE) const;

	bool IsWetAt(const FVector2D& World, int32 Layer = INDEX_NONE) const;

	/** Every drop in the field, in cubic cm. What the conservation test asserts on. */
	double GetTotalVolume() const;

	/**
	 * The same, bounded to a disc.
	 *
	 * HOW A PROXY MEASURES ITSELF without keeping a copy of its own cell list. A
	 * region asks before and after a reaction takes water off it, and the
	 * difference is what actually went -- which is the only way to answer "is
	 * there still a body here" that cannot go stale between reconciles.
	 */
	double GetVolumeWithin(const FVector2D& Centre, float Radius, int32 Layer = INDEX_NONE) const;

	/** Ground actually under water inside that disc, in square cm. */
	double GetWetAreaWithin(const FVector2D& Centre, float Radius, int32 Layer = INDEX_NONE) const;

	// --- Working on ONE body's cells -------------------------------------------
	//
	// WHY A BODY CANNOT BE ADDRESSED BY A LAYER NUMBER. A layer index is
	// allocation order at ONE cell: a cell that first met water on the balcony
	// holds the balcony as layer 0 and the ground as layer 1, and the cell beside
	// it that only ever saw the ground holds it the other way round. That is fine
	// for the solver, which matches BEDS between neighbours and is exact at one
	// cell's remove, and useless for anything talking about a whole body at once.
	//
	// Matching one representative HEIGHT instead fails the other way: a puddle
	// running down a long ramp spans far more than a LayerSeparation, so no single
	// height stands for it.
	//
	// So a body is addressed by the cells it actually holds -- which the reconcile
	// worked out, and which are exact. These four are the operations that used to
	// take a disc and now take the body itself.

	using FCellSet = TSet<TPair<FIntPoint, int32>>;

	// --- Handing the field to the GPU ------------------------------------------

	/**
	 * A square of world the presentation layer wants a picture of.
	 *
	 * A WINDOW, NOT THE WORLD. The field is unbounded and mostly dry; what a
	 * shader can afford is a few metres around the viewer at a resolution fine
	 * enough to be worth sampling. Everything outside is drawn by nothing, which
	 * is correct -- there is no water there worth a pixel.
	 */
	struct FWindow
	{
		/** World XY of the window's minimum corner. */
		FVector2D Origin = FVector2D::ZeroVector;

		/** How wide the window is, in cm. */
		float Extent = 2048.f;

		/** Texels across. The texture is this square. */
		int32 Resolution = 128;

		/**
		 * Which floor the picture is of.
		 *
		 * THE VIEWER'S OWN. A balcony and the room under it are the same square of
		 * ground, and a flat picture can only show one of them -- so it shows the
		 * one the player is standing on. Anything else would draw the puddle
		 * upstairs onto the floor downstairs.
		 */
		float NearZ = 0.f;
	};

	/**
	 * Writes the window into a pixel buffer for upload.
	 *
	 * THE CHANNELS ARE A CONTRACT with the Niagara sheet and with the surface
	 * material, and they are written out in Docs/FLUID_NIAGARA_AUTHORING.md as
	 * well as here because the other end of them lives in an asset no script can
	 * read:
	 *
	 *   R  depth of fluid, in cm
	 *   G  the floor, in world Z
	 *   B  the top of anything SOLID standing here, or the floor where nothing is
	 *   A  1 where fluid may not enter, 0 where it may
	 *
	 * WHY B IS NOT JUST THE SOLID. A shallow-water sim wants one bottom contour,
	 * and to it a slab standing on the floor is simply floor that is higher up.
	 * Handing it max(floor, solid) means water parts around a wall with no code in
	 * the graph that knows what a wall is.
	 *
	 * SAMPLED PER TEXEL RATHER THAN COPIED PER CELL, so the window's resolution
	 * and the field's are independent -- and they are: the field is 40cm because
	 * that is what gameplay needs to be exact about, and the picture is whatever
	 * the material can afford to sample.
	 */
	/**
	 * @param Only  When given, only these cells report water; everything else is
	 *              written dry. The FLOOR is still reported everywhere, because
	 *              the sheet drapes onto it and a hole in the floor is a tear.
	 *
	 * ONE BODY AT A TIME. Sheets are sized to a body plus an apron, so two bodies
	 * lying near each other have overlapping boxes -- and a picture taken from the
	 * shared field puts BOTH bodies' water into BOTH pictures. Two sheets then
	 * draw the same water at the same height: masked fluids z-fight and translucent
	 * ones double up, which reads as dark patches flashing across a puddle the
	 * moment it stops spreading and settles beside its neighbours.
	 */
	void SampleWindow(const FWindow& Window, TArray<FFloat16Color>& OutPixels,
		const FCellSet* Only = nullptr) const;

	/** Every drop held by these slots, in cubic cm. */
	double GetVolumeOfCells(const FCellSet& InCells) const;

	/** Ground under water in these slots, in square cm. */
	double GetAreaOfCells(const FCellSet& InCells) const;

	/**
	 * Takes a volume out of these slots, nearest a point first.
	 *
	 * NEAREST FIRST because what asks is a reaction that happened somewhere.
	 * Spreading the loss evenly would take water off the far rim of a puddle a
	 * fireball touched the near edge of.
	 */
	double RemoveFromCells(const FCellSet& InCells, const FVector2D& Near, double Volume);

	/**
	 * Empties whole slots nearest a point until that much ground is dry.
	 *
	 * GROUND, NOT VOLUME, and the difference is the whole reason this exists
	 * beside RemoveFromCells. A reaction's spend converts into AREA -- that is
	 * what IARPGElementalSurface::GetSurfaceEnergyDensity means -- and turning
	 * that area back into a volume to remove requires guessing how deep the water
	 * is. The nominal depth is only ever approximately right, because a rasterised
	 * disc is not quite the disc it was poured as, so a fireball worth exactly one
	 * puddle removed 98% of one and left a rim that never went.
	 *
	 * Taking cells outright has no such error: the area removed is the area asked
	 * for, to within one cell.
	 *
	 * @return the ground actually taken, in square cm.
	 */
	double RemoveAreaFromCells(const FCellSet& InCells, const FVector2D& Near, double Area);

	/** Empties whichever of these slots a polygon covers, and stamps them frozen. */
	double ConsumeCellsIn(const FCellSet& InCells, const TArray<FVector2D>& Region);

	int32 GetWetCellCount() const;
	int32 GetChunkCount() const { return Chunks.Num(); }

	// --- Stepping ---------------------------------------------------------------

	/**
	 * One fixed step.
	 *
	 * MASS IS CONSERVED BY CONSTRUCTION rather than by care: every transfer is
	 * subtracted from one cell and added to another in the same pass, and a cell
	 * that would give away more than it has has all of its outflows scaled down
	 * together. There is no path through this function that creates or destroys a
	 * drop, which is why the test for it is an equality and not a tolerance.
	 */
	void Step(float DeltaTime);

	/**
	 * Grows or shrinks every wet cell, for rain and evaporation.
	 *
	 * SEPARATE FROM Step, and separate because this is the one thing that is
	 * ALLOWED to change the total. Folding it into the solver would make the
	 * conservation test untestable, which is the same argument for keeping it out
	 * of the flux pass that keeps the flux pass honest.
	 *
	 * @return the volume gained (positive) or lost (negative).
	 */
	double ApplyWeather(float DepthPerSecond, float DeltaTime);

	// --- Regions ----------------------------------------------------------------

	/** One connected body of wet cells. What a pool used to be an actor for. */
	struct FRegion
	{
		/** Wet cells in this body, as (chunk, index) pairs. */
		TArray<TPair<FIntPoint, int32>> Cells;

		/**
		 * The same cells, for asking whether one is in this body.
		 *
		 * Carried alongside the list rather than derived on demand because the
		 * outline tracer asks it four times per cell, and a body matched to its
		 * proxy asks it once per proxy per pass.
		 */
		TSet<TPair<FIntPoint, int32>> Lookup;

		/**
		 * A cell that is definitely in this body, nearest its middle.
		 *
		 * WHAT A PROXY IS MATCHED BY between one pass and the next. The centroid
		 * itself will not do: a C-shaped puddle round a wall has its centroid in
		 * the wall, and matching on a point outside the body would retire and
		 * respawn the proxy every pass -- taking whatever was floating on it with
		 * it, since AARPGSolidBody::FloatsOn points at one.
		 */
		/**
		 * Which floor this body is on.
		 *
		 * A BODY IS ALL ON ONE, necessarily: flow only ever connects slots in the
		 * same layer, so a connected component cannot straddle two. Carried so the
		 * proxy standing for it can ask the field questions about the right one --
		 * IARPGElementalSurface takes an FVector2D and has no Z to disambiguate
		 * with, which is exactly why the layer lives on the body rather than in
		 * the query.
		 */
		int32 Layer = 0;

		FVector2D Centroid = FVector2D::ZeroVector;
		FBox2D Bounds = FBox2D(ForceInit);
		double Volume = 0.0;
		double Area = 0.0;

		/** Highest surface in the body, for a trigger box that has to contain it. */
		float MaxLevel = 0.f;
		float MinBed = 0.f;
	};

	/**
	 * Labels every connected body of wet cells.
	 *
	 * FOUR-CONNECTED, matching the solver: fluid moves between face neighbours
	 * only, so two cells touching at a corner are not one body by any route the
	 * simulation can take, and calling them one would merge two puddles that
	 * cannot reach each other.
	 */
	void FindRegions(TArray<FRegion>& OutRegions) const;

	/**
	 * The outline of one region, as a single simple ring in world XY.
	 *
	 * WHY A POLYGON AT ALL, when the whole point of the field was to stop being
	 * one. Because the things that ask are not the simulation: TrySolidify clips a
	 * spell's circle against the water it overlaps and hands the result to
	 * IntersectWithHoles, and a slab is still an outline extruded to a thickness.
	 * Those want a shape. The difference is that this one is DERIVED -- worked out
	 * from the cells whenever somebody asks -- rather than being the thing the
	 * water actually is, so it can be thrown away and rebuilt and never drifts.
	 *
	 * TRACED ALONG CELL EDGES, so the ring is axis-aligned and exact rather than
	 * smoothed. A smoothed contour would be prettier and would disagree with
	 * IsWetAt about the cells along its border, which is the one thing a shape
	 * used for clipping must not do.
	 *
	 * COLLINEAR RUNS COLLAPSED, because a two-metre straight edge is one segment
	 * and not five, and every consumer of this walks its vertices.
	 *
	 * The LARGEST loop only. A region with a slab standing in the middle of it has
	 * an inner loop too, and a fluid has never carried one -- water flows back over
	 * a gap, and the gap here is a blocked cell the solver already refuses to enter.
	 */
	void BuildRegionOutline(const FRegion& Region, TArray<FVector2D>& OutRing) const;

	/**
	 * Which cell a world XY falls in.
	 *
	 * Public because a proxy has to be able to ask whether a point is one of ITS
	 * cells, and the chunking is the field's business rather than something every
	 * caller should reproduce.
	 */
	void ResolveCellPublic(const FVector2D& World, FIntPoint& OutCoord, int32& OutIndex) const
	{
		ResolveCell(World, OutCoord, OutIndex);
	}

	/** The region containing a world XY, or INDEX_NONE. */
	int32 FindRegionAt(const TArray<FRegion>& Regions, const FVector2D& World) const;

private:
	/** Chunk coordinate and cell index for a world XY. Always succeeds. */
	void ResolveCell(const FVector2D& World, FIntPoint& OutCoord, int32& OutIndex) const;

	/**
	 * The same, from a GLOBAL cell coordinate rather than a position.
	 *
	 * WHICH IS THE HONEST ADDRESS. Chunks are an allocation strategy, not part of
	 * the model -- a cell two steps east is two steps east whether or not that
	 * crosses an edge -- so everything that walks the grid works in global cells
	 * and this is the one place the chunking is reintroduced.
	 */
	void ResolveGlobal(int32 GlobalX, int32 GlobalY, FIntPoint& OutCoord, int32& OutIndex) const;

	/** Global cell coordinates containing a world XY. */
	FIntPoint GlobalCellAt(const FVector2D& World) const;

	/** World XY of a cell's centre. */
	FVector2D CellCentre(FIntPoint Coord, int32 Index) const;

	FARPGFluidChunk* FindChunk(FIntPoint Coord);
	const FARPGFluidChunk* FindChunk(FIntPoint Coord) const;

	/** Allocates on demand. The world is mostly dry, so most coordinates have none. */
	FARPGFluidChunk& FindOrAddChunk(FIntPoint Coord);

	/**
	 * Traces the floor under a cell, once, and remembers it.
	 *
	 * CACHED FOREVER because level geometry does not move, and a puddle that only
	 * ever shrinks would otherwise pay for the same trace on every step for the
	 * rest of its life. The same argument AARPGFluidPool::SampleBed already makes
	 * for its own grid, one level up.
	 */
	/**
	 * The layer at this cell whose floor is nearest a height, or INDEX_NONE.
	 *
	 * ASKS ONLY WHAT IS ALREADY KNOWN -- no trace, no allocation. What a query
	 * uses, and what the solver uses to find which of a neighbour's floors is the
	 * continuation of its own.
	 */
	int32 FindLayerNear(const FARPGFluidChunk& Chunk, int32 Cell, float NearZ) const;

	/** The layer holding water at this cell, preferring the most of it. */
	int32 FindWetLayer(const FARPGFluidChunk& Chunk, int32 Cell) const;

	/** Which slot a query about this cell means, given a layer or none. */
	int32 SlotForQuery(const FARPGFluidChunk& Chunk, int32 Cell, int32 Layer) const;

	/**
	 * The layer for a height, tracing and allocating one if there is none.
	 *
	 * PROBES FIRST, MATCHES SECOND, and the order is what stops a cliff growing a
	 * second layer that duplicates the first. Water arriving at the top of a drop
	 * asks for a floor near the TOP; the trace answers with the floor at the
	 * BOTTOM; and it is that answer, not the question, that has to be matched
	 * against the layers already there.
	 *
	 * @return the layer, or INDEX_NONE where there is no floor or no layer free.
	 */
	int32 EnsureLayer(FIntPoint Coord, int32 Cell, float NearZ, float& OutBed);

	void MarkActive(FARPGFluidChunk& Chunk, int32 Index);
	void MarkNextActive(FARPGFluidChunk& Chunk, int32 Index);

	/** Wakes a cell and its four neighbours, across a chunk edge if need be. */
	void WakeAround(FIntPoint Coord, int32 Index);

	/** The neighbour one step away, which may be in the chunk next door. */
	void StepCell(FIntPoint Coord, int32 Index, int32 DX, int32 DY,
		FIntPoint& OutCoord, int32& OutIndex) const;

	float ChunkSize = 3840.f;
	int32 Resolution = 96;

	/**
	 * How many floors one cell may hold water on. See the class comment.
	 *
	 * TWO, because that is the shape of the problem rather than a budget: a
	 * balcony over a floor, a bridge over a stream, a gantry over a vat. Three
	 * stacked water surfaces in one column is not a level anybody builds, and
	 * every extra layer costs a full grid whether or not anything is ever on it.
	 */
	int32 Layers = 2;

	FARPGFluidFieldParams Params;
	FBedProbe BedProbe;

	/** Coord to grids. A map, because the world is mostly not wet. */
	TMap<FIntPoint, FARPGFluidChunk> Chunks;

	/** Scratch, kept between steps so a step does not allocate. */
	TArray<FARPGFluidCrossDeposit> CrossDeposits;
	TArray<float> DepthDelta;
	TArray<float> FluxX;
	TArray<float> FluxY;
	TArray<TPair<FIntPoint, FARPGFluidChunk*>> Wet;
};
