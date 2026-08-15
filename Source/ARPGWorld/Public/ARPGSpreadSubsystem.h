// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"
#include "ARPGSpreadSubsystem.generated.h"

class UARPGFuelComponent;

class UARPGMagicCombinationTable;
class UARPGSpreadDefinition;
class UARPGSpreadFuelMap;
class UMaterialParameterCollection;

/**
 * Simulates every DIFFUSIVE spreadable medium -- fire, corruption, pestilence --
 * on one budgeted tick. Port of Godot's SpreadSystem.
 *
 * TWO COUPLED LAYERS, and a medium may use either or both:
 *
 *   OBJECT LAYER  Discrete registered targets (trees, props, characters).
 *                 Afflicted targets push exposure at nearby ones through a
 *                 uniform spatial hash. "Afflicted" is NOT state kept here: it
 *                 is whether the target carries the medium's status effect, so
 *                 the status system stays the single source of truth and
 *                 anything that applies or strips one -- a fireball's on-hit
 *                 effects, a Wet status removing Burning -- steers the spread
 *                 for free.
 *
 *   FIELD LAYER   A per-chunk cellular grid carrying the medium across open
 *                 ground between discrete objects. This is what makes a grass
 *                 fire cross a CLEARING rather than stopping at each tree.
 *
 * Design notes, in the order they matter for performance:
 *
 *   - Field grids are allocated PER CHUNK on demand and freed when a chunk goes
 *     inert. One mask sliding with the player is the obvious alternative and is
 *     wrong: it has to move the whole grid as the player walks, and silently
 *     drops any fire that leaves the window.
 *
 *   - Each tick visits only cells in an ACTIVE SET plus their one-ring, never
 *     the whole grid. A map-wide firestorm and a single burning bush each cost
 *     in proportion to what is actually alight.
 *
 *   - Environmental damage spawns NO hitboxes and no per-cell volumes. There are
 *     few characters and potentially thousands of hot cells, so the query runs
 *     the cheap way round: walk the registered targets and sample the field
 *     under each. Damage goes through the hurtbox, so i-frames apply and dodging
 *     through fire works exactly as dodging through a sword does.
 *
 * Ticked at a fixed rate rather than per frame. Diffusion is slow and visually
 * forgiving; 10Hz is plenty and keeps the cost off the render frame.
 */
UCLASS()
class ARPGWORLD_API UARPGSpreadSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Media registered at once. Fixed, so per-target accumulators stay flat. */
	static constexpr int32 MaxMedia = 8;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	// --- Configuration --------------------------------------------------------

	/**
	 * Registered media. Entries with no element are rejected with a warning.
	 *
	 * Populated from UARPGWorldSettings on Initialize. A test may assign it
	 * directly instead -- anything already present when Initialize runs is left
	 * alone, so a fixture never has to fight the project settings.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Spread")
	TArray<TObjectPtr<UARPGSpreadDefinition>> Definitions;

	/** The SAME table every other solver reads. Drives cross-medium attrition. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Spread")
	TObjectPtr<UARPGMagicCombinationTable> CombinationTable;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Spread")
	TObjectPtr<UARPGSpreadFuelMap> FuelMap;

	/** Simulation ticks per second. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Spread",
		meta = (ClampMin = "1.0", ClampMax = "60.0"))
	float TickRate = 10.f;

	/** Chunk edge in centimetres. Must match the fuel map's bake. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Spread",
		meta = (ClampMin = "100.0"))
	float ChunkSize = 15360.f;

	/** Cells per chunk edge. 64 over a 153.6m chunk gives 2.4m cells. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Spread",
		meta = (ClampMin = "4", ClampMax = "256"))
	int32 FieldResolution = 64;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Spread|Wind")
	FVector2D WindDirection = FVector2D(1.f, 0.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Spread|Wind",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WindStrength = 0.f;

	// --- Seeding --------------------------------------------------------------

	/**
	 * Deposits exposure into the field over a disc. The entry point for
	 * everything that starts a fire: a spell, a hazard, a script.
	 *
	 * @return how much was actually deposited, which is less than asked for
	 *         where the ground has no fuel.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Spread")
	float AddExposure(FVector WorldPosition, float Radius, float Amount, FGameplayTag ElementTag);

	/** Removes intensity over a disc -- a dispel, or a bucket of water. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Spread")
	void Extinguish(FVector WorldPosition, float Radius, FGameplayTag ElementTag);

	/** Overrides baked fuel over a disc, for runtime terrain changes. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Spread")
	void SetFieldFuel(FVector WorldPosition, float Radius, float FuelFraction, FGameplayTag ElementTag);

	// --- Queries --------------------------------------------------------------

	/** Current intensity at a point, 0 when nothing is there. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Spread")
	float GetFieldIntensity(FVector WorldPosition, FGameplayTag ElementTag) const;

	/** How burnt the ground is, 0-1. Monotonic; what the char shader reads. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Spread")
	float GetFieldResidue(FVector WorldPosition, FGameplayTag ElementTag) const;

	/** Remaining fuel at a point, 0-1. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Spread")
	float GetFieldFuel(FVector WorldPosition, FGameplayTag ElementTag) const;

	/** Unspent energy bank at a point -- what bounds how much further it reaches. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Spread")
	float GetFieldEnergy(FVector WorldPosition, FGameplayTag ElementTag) const;

	/** Is this point burning, rather than merely warm? */
	UFUNCTION(BlueprintPure, Category = "ARPG|Spread")
	bool IsBurning(FVector WorldPosition, FGameplayTag ElementTag) const;

	/** How many cells are currently alight. What a test asserts reach on. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Spread")
	int32 GetBurningCellCount(FGameplayTag ElementTag) const;

	/** Runs one simulation step immediately, bypassing the tick rate. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Spread")
	void StepSimulation(float DeltaTime);

	// --- Targets --------------------------------------------------------------

	/** Registers an actor as a discrete spread target. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Spread")
	void RegisterTarget(AActor* Actor);

	UFUNCTION(BlueprintCallable, Category = "ARPG|Spread")
	void UnregisterTarget(AActor* Actor);

	// --- Objects that are fuel ---------------------------------------------------
	//
	// THE GROUND BURNS AND OBJECTS DID NOT. Fuel is baked per cell from the world,
	// so a wooden crate on bare stone was scenery the fire went round. A registered
	// fuel component is the other half: something that adds to the fire where it
	// stands and has a finite amount of itself to add.
	//
	// IT KEEPS ITS OWN FUEL. Mixing an object's fuel into the field's cells would
	// lose track of whose is whose -- quenching would refund the crate, and burning
	// the grass would consume it. So the field says only whether a cell is alight,
	// and the object answers with what it is prepared to give.

	UFUNCTION(BlueprintCallable, Category = "ARPG|Spread")
	void RegisterFuel(UARPGFuelComponent* Fuel);

	UFUNCTION(BlueprintCallable, Category = "ARPG|Spread")
	void UnregisterFuel(UARPGFuelComponent* Fuel);

private:
	/** One medium, resolved once from its definition. */
	struct FMedium
	{
		TObjectPtr<UARPGSpreadDefinition> Definition;
		FGameplayTag ElementTag;
		int32 FieldSlot = INDEX_NONE;
	};

	/**
	 * One chunk's grids. Parallel flat arrays per medium slot, not an array of
	 * per-cell structs: the tick touches one field at a time across many cells,
	 * so this is the layout the access pattern actually wants.
	 */
	struct FFieldChunk
	{
		TArray<float> Intensity[MaxMedia];
		TArray<float> Fuel[MaxMedia];
		TArray<float> Residue[MaxMedia];
		TArray<float> Energy[MaxMedia];
		bool bSlotUsed[MaxMedia] = {};

		/** Cells worth visiting. Rebuilt each tick from what stayed hot. */
		TArray<int32> Active;
		TArray<int32> NextActive;
		TArray<uint8> ActiveStamp;

		/**
		 * Scorch marks on this chunk's ground, and how many cells carry them.
		 *
		 * A count rather than a flag. The flag was set the first time anything
		 * burned and never cleared, and the inert-chunk sweep refuses to free a
		 * chunk that has residue -- so every chunk that had ever seen fire was
		 * retained for the rest of the session, which is the opposite of what
		 * this class documents. Residue is monotonic per cell, so the count only
		 * grows; what it buys is an exact answer to "does this chunk still have
		 * anything worth keeping" for a chunk that never actually caught.
		 */
		int32 ResidueCells = 0;
	};

	/** A deposit that crossed a chunk edge, resolved after the pass. */
	struct FCrossDeposit
	{
		FIntPoint Coord;
		int32 Index = 0;
		int32 Medium = 0;
		float Amount = 0.f;
		float Energy = 0.f;
	};

	/**
	 * Per-chunk working state, so the sweep can run in parallel.
	 *
	 * ONE SLOT PER CHUNK, kept between ticks. The two things a chunk step used to
	 * share were a scratch list of active cells and the cross-boundary deposit
	 * queue -- both write-heavy, and both races the moment more than one chunk
	 * runs at once. Held here rather than allocated per chunk per tick, which is
	 * what the shared scratch existed to avoid in the first place.
	 */
	struct FChunkWork
	{
		TArray<int32> Active;
		TArray<FCrossDeposit> Deposits;

		/**
		 * The medium pass's accumulators, one cell each.
		 *
		 * PER SLOT LIKE THE REST. These were shared too, and they are the write
		 * that actually matters: a chunk step accumulates every neighbour's
		 * contribution into them before applying, so two chunks sharing a pair
		 * would not merely race, they would spread each other's fire.
		 */
		TArray<float> DeltaIntensity;
		TArray<float> DeltaEnergy;
	};

	/**
	 * Builds the media list if the definitions have changed since last time.
	 *
	 * Called by EVERY entry point, including the const queries -- which is why
	 * the cache is mutable. Rebuilding only on seeding was a real bug: setting
	 * fuel before the first seed silently did nothing, because the medium it
	 * named did not exist yet, and a firebreak painted that way simply burned.
	 */
	void EnsureMedia() const;

	int32 FindMedium(FGameplayTag ElementTag) const;

	float GetCellSize() const { return ChunkSize / FMath::Max(1, FieldResolution); }

	/** Chunk coordinate and cell index for a world position. */
	bool ResolveCell(FVector WorldPosition, FIntPoint& OutCoord, int32& OutIndex) const;

	FFieldChunk* FindChunk(FIntPoint Coord);
	const FFieldChunk* FindChunk(FIntPoint Coord) const;

	/** Allocates on demand, seeding fuel from the baked map. */
	FFieldChunk& FindOrAddChunk(FIntPoint Coord);

	void EnsureSlot(FFieldChunk& Chunk, int32 Slot, FIntPoint Coord);

	void MarkActive(FFieldChunk& Chunk, int32 Index);
	void MarkNextActive(FFieldChunk& Chunk, int32 Index);

	void TickField(float DeltaTime);
	/**
	 * One chunk's step. TOUCHES NOTHING OUTSIDE ITS OWN CHUNK AND ITS OWN SCRATCH,
	 * which is what makes the sweep over chunks parallel: it reads shared
	 * configuration, writes its own cells, and posts anything crossing a boundary
	 * to the work slot for the caller to apply afterwards.
	 */
	void TickFieldChunk(FIntPoint Coord, FFieldChunk& Chunk, float DeltaTime,
		FChunkWork& Work);
	void TickAttrition(FIntPoint Coord, FFieldChunk& Chunk, float DeltaTime);
	void TickContactDamage(float DeltaTime);

	/** Deposits a cross-chunk transfer once the pass is done. */
	void ApplyCrossDeposits();

	/**
	 * Burns whatever objects are standing in fire, and lets them feed it back.
	 *
	 * AFTER the field step and before contact damage. After, because whether an
	 * object is alight is a question about the field as it now is; before, because
	 * an object that just caught should be hurting whoever is leaning on it in the
	 * same tick.
	 */
	void TickFuelSources(float DeltaTime);

	void RebuildMedia() const;
	void RebuildAttritionRates() const;

	/** Directional weight for one neighbour step under the current wind. */
	float GetWindWeight(FVector2D StepDirection, float Bias) const;

	/**
	 * True when this machine may resolve the simulation and deal its damage.
	 *
	 * A world subsystem ticks on clients too, and nothing here was gated: every
	 * client ran its own unreplicated fire field and applied contact damage from
	 * it, burning real invincibility frames against a hazard the server had never
	 * agreed existed.
	 */
	bool HasAuthority() const;

	mutable TArray<FMedium> Media;

	/** Coord to grids. A map rather than a grid: the world is mostly not alight. */
	TMap<FIntPoint, FFieldChunk> Chunks;

	TArray<FCrossDeposit> CrossDeposits;


	TArray<FChunkWork> ChunkWork;

	/** The chunks with anything alight, flattened so the sweep can index them. */
	TArray<TPair<FIntPoint, FFieldChunk*>> Burning;

	/**
	 * Scratch reused by every chunk, every medium, every tick.
	 *
	 * These used to be two TMaps and an array constructed inside the per-medium
	 * loop, so a modest fire allocated and freed several heap blocks per chunk
	 * per medium at 10Hz. Indexed by cell, sized once to the grid, and cleared by
	 * walking only the cells actually touched -- which is why the touch lists
	 * exist rather than a Memset over the whole grid.
	 */
	TArray<int32> ScratchTouchedIntensity;
	TArray<int32> ScratchTouchedEnergy;
	/** Sizes the scratch buffers to the current grid, once. */
	void EnsureScratch();

	/**
	 * How fast medium A is destroyed per unit of medium B present, derived from
	 * the combination table's Field-scope rows.
	 *
	 * Precomputed rather than looked up per cell: this is read for every pair of
	 * co-located media on every active cell, and the table lookup is a container
	 * comparison.
	 */
	mutable float AttritionRates[MaxMedia][MaxMedia] = {};

	float TickAccumulator = 0.f;

	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<AActor>> Targets;

	/** Objects that feed a medium and are consumed doing it. */
	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<UARPGFuelComponent>> FuelSources;

	mutable bool bMediaDirty = true;
};
