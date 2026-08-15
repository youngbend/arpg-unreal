// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGSpreadSubsystem.h"
#include "ARPGDamageGameplayEffect.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGGameplayEffectContext.h"
#include "ARPGGameplayTags.h"
#include "ARPGHurtboxComponent.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicElement.h"
#include "ARPGSpreadDefinition.h"
#include "ARPGFuelComponent.h"
#include "ARPGSpreadFuelMap.h"
#include "ARPGWorld.h"
#include "ARPGWorldSettings.h"
#include "Engine/Texture2D.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Engine/World.h"
#include "GameplayEffect.h"

namespace
{
	/** Below this a cell is cold and drops out of the active set. */
	constexpr float ColdEpsilon = 0.001f;

	/** Orthogonal neighbours are closer, so they receive more. */
	constexpr float OrthoWeight = 1.f;
	constexpr float DiagonalWeight = 0.7071f;
	constexpr float InvSqrt2 = 0.7071f;

	/**
	 * How much of a transfer flows into an already-burning neighbour as BANK
	 * rather than exposure.
	 *
	 * Without it a strongly seeded patch strands its energy one ring behind the
	 * front: interior cells never spend, because all their neighbours are
	 * already alight, while the frontier -- lit by falloff scraps -- goes broke
	 * immediately and the fire stalls at its own rim.
	 */
	constexpr float ConductionScale = 0.5f;
}

void UARPGSpreadSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Project settings fill in anything not already assigned. A test that has set
	// Definitions on the subsystem directly therefore keeps them, and a real
	// session gets a configured simulation instead of an inert one.
	const UARPGWorldSettings& Settings = UARPGWorldSettings::Get();

	if (Definitions.Num() == 0)
	{
		for (const TSoftObjectPtr<UARPGSpreadDefinition>& Soft : Settings.SpreadDefinitions)
		{
			if (UARPGSpreadDefinition* Definition = Soft.LoadSynchronous())
			{
				Definitions.Add(Definition);
			}
			else if (!Soft.IsNull())
			{
				UE_LOG(LogARPGWorld, Warning,
					TEXT("Spread definition '%s' from project settings failed to load."),
					*Soft.ToString());
			}
		}
	}

	if (!CombinationTable)
	{
		CombinationTable = Settings.CombinationTable.LoadSynchronous();
	}

	if (!FuelMap)
	{
		FuelMap = Settings.FuelMap.LoadSynchronous();
	}

	bMediaDirty = true;
}

bool UARPGSpreadSubsystem::HasAuthority() const
{
	const UWorld* World = GetWorld();

	// A standalone or listen-server world is authoritative; a pure client is not.
	// Anything with no net driver at all -- an automation fixture -- counts as
	// authoritative, because there is nobody else to be.
	return !World || World->GetNetMode() != NM_Client;
}

bool UARPGSpreadSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UARPGSpreadSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UARPGSpreadSubsystem, STATGROUP_Tickables);
}

// ---------------------------------------------------------------------------
// Media
// ---------------------------------------------------------------------------

void UARPGSpreadSubsystem::EnsureMedia() const
{
	if (bMediaDirty)
	{
		RebuildMedia();
	}
}

void UARPGSpreadSubsystem::RebuildMedia() const
{
	Media.Reset();

	int32 NextFieldSlot = 0;

	for (UARPGSpreadDefinition* Definition : Definitions)
	{
		if (!Definition)
		{
			continue;
		}

		const FGameplayTag Tag = Definition->GetElementTag();
		if (!Tag.IsValid())
		{
			// Rejected loudly rather than silently doing nothing: a medium with
			// no identity cannot be seeded, matched or queried, and would look
			// exactly like a simulation bug.
			UE_LOG(LogARPGWorld, Warning,
				TEXT("Spread definition '%s' has no element tag and was not registered."),
				*GetNameSafe(Definition));
			continue;
		}

		if (Media.Num() >= MaxMedia)
		{
			UE_LOG(LogARPGWorld, Warning,
				TEXT("More than %d spread media registered; '%s' was dropped."),
				MaxMedia, *Tag.ToString());
			break;
		}

		FMedium Medium;
		Medium.Definition = Definition;
		Medium.ElementTag = Tag;
		Medium.FieldSlot = Definition->bUsesField ? NextFieldSlot++ : INDEX_NONE;
		Media.Add(Medium);
	}

	RebuildAttritionRates();
	bMediaDirty = false;
}

void UARPGSpreadSubsystem::RebuildAttritionRates() const
{
	FMemory::Memzero(AttritionRates, sizeof(AttritionRates));

	if (!CombinationTable)
	{
		return;
	}

	// Derived from the SHARED table, so rain quenching a grass fire and a water
	// jet meeting a fireball read identical numbers -- one authored row, two
	// solvers. Precomputed because this is consulted for every pair of
	// co-located media on every active cell.
	for (int32 A = 0; A < Media.Num(); ++A)
	{
		for (int32 B = 0; B < Media.Num(); ++B)
		{
			if (A == B)
			{
				continue;
			}

			FGameplayTagContainer Pair;
			Pair.AddTag(Media[A].ElementTag);
			Pair.AddTag(Media[B].ElementTag);

			const UARPGMagicCombinationEntry* Entry =
				CombinationTable->ResolveEntry(Pair, EARPGCombinationScope::Field);
			if (!Entry)
			{
				continue;
			}

			// A's consumption rate is how fast A is destroyed by meeting B.
			// Asymmetry is the point: water quenches fire far faster than fire
			// boils off water, and both numbers live on the same row.
			AttritionRates[A][B] = Entry->GetConsumptionRate(Media[A].ElementTag);
		}
	}
}

int32 UARPGSpreadSubsystem::FindMedium(FGameplayTag ElementTag) const
{
	EnsureMedia();

	for (int32 Index = 0; Index < Media.Num(); ++Index)
	{
		if (Media[Index].ElementTag == ElementTag)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

// ---------------------------------------------------------------------------
// Chunks
// ---------------------------------------------------------------------------

bool UARPGSpreadSubsystem::ResolveCell(FVector WorldPosition, FIntPoint& OutCoord, int32& OutIndex) const
{
	if (FieldResolution <= 0 || ChunkSize <= 0.f)
	{
		return false;
	}

	OutCoord = FIntPoint(
		FMath::FloorToInt(WorldPosition.X / ChunkSize),
		FMath::FloorToInt(WorldPosition.Y / ChunkSize));

	// Local offset within the chunk, kept positive across the origin -- fmod
	// alone goes negative for negative coordinates and mirrors the whole grid.
	const float LocalX = WorldPosition.X - OutCoord.X * ChunkSize;
	const float LocalY = WorldPosition.Y - OutCoord.Y * ChunkSize;

	const int32 CellX = FMath::Clamp(
		FMath::FloorToInt(LocalX / GetCellSize()), 0, FieldResolution - 1);
	const int32 CellY = FMath::Clamp(
		FMath::FloorToInt(LocalY / GetCellSize()), 0, FieldResolution - 1);

	OutIndex = CellY * FieldResolution + CellX;
	return true;
}

UARPGSpreadSubsystem::FFieldChunk* UARPGSpreadSubsystem::FindChunk(FIntPoint Coord)
{
	return Chunks.Find(Coord);
}

const UARPGSpreadSubsystem::FFieldChunk* UARPGSpreadSubsystem::FindChunk(FIntPoint Coord) const
{
	return Chunks.Find(Coord);
}

UARPGSpreadSubsystem::FFieldChunk& UARPGSpreadSubsystem::FindOrAddChunk(FIntPoint Coord)
{
	if (FFieldChunk* Existing = Chunks.Find(Coord))
	{
		return *Existing;
	}

	FFieldChunk& Chunk = Chunks.Add(Coord);
	Chunk.ActiveStamp.SetNumZeroed(FieldResolution * FieldResolution);
	return Chunk;
}

void UARPGSpreadSubsystem::EnsureSlot(FFieldChunk& Chunk, int32 Slot, FIntPoint Coord)
{
	if (Slot < 0 || Slot >= MaxMedia || Chunk.bSlotUsed[Slot])
	{
		return;
	}

	const int32 CellCount = FieldResolution * FieldResolution;

	Chunk.Intensity[Slot].SetNumZeroed(CellCount);
	Chunk.Residue[Slot].SetNumZeroed(CellCount);
	Chunk.Energy[Slot].SetNumZeroed(CellCount);
	Chunk.Fuel[Slot].SetNumUninitialized(CellCount);

	// Fuel is seeded from the BAKED map at allocation, once, rather than sampled
	// per lookup -- which is what makes a painted firebreak a property of the
	// world rather than something the tick rediscovers every step.
	const UARPGSpreadDefinition* Definition = nullptr;
	for (const FMedium& Medium : Media)
	{
		if (Medium.FieldSlot == Slot)
		{
			Definition = Medium.Definition;
			break;
		}
	}

	const float FuelSeconds = Definition ? Definition->FuelSeconds : 1.f;

	for (int32 Y = 0; Y < FieldResolution; ++Y)
	{
		for (int32 X = 0; X < FieldResolution; ++X)
		{
			const float NormalX = (X + 0.5f) / FieldResolution;
			const float NormalY = (Y + 0.5f) / FieldResolution;

			// THE GROUND SAYS WHAT IT IS AND THE MEDIUM SAYS WHAT THAT IS WORTH.
			// One byte per cell as before, but it now names a surface rather than
			// an amount -- so a marsh can refuse fire and carry a frost, which one
			// element-agnostic number could never express.
			float Fraction = 1.f;

			if (FuelMap && Definition)
			{
				const uint8 Surface = FuelMap->GetSurfaceAt(Coord, NormalX, NormalY);

				// And jittered, because a uniform grid burns in diamonds: every
				// cell in a ring reaches ignition on the same tick, so the front is
				// a shape the grid chose. Baked, so a patch of ground always burns
				// the same way -- ragged, not random.
				Fraction = Definition->GetSurfaceFuel(Surface)
					* FuelMap->GetJitterAt(Coord, NormalX, NormalY);
			}

			Chunk.Fuel[Slot][Y * FieldResolution + X] =
				FMath::Max(0.f, Fraction) * FuelSeconds;
		}
	}

	Chunk.bSlotUsed[Slot] = true;
}

void UARPGSpreadSubsystem::MarkActive(FFieldChunk& Chunk, int32 Index)
{
	if (!Chunk.ActiveStamp.IsValidIndex(Index) || Chunk.ActiveStamp[Index] != 0)
	{
		return;
	}

	Chunk.ActiveStamp[Index] = 1;
	Chunk.Active.Add(Index);
}

void UARPGSpreadSubsystem::MarkNextActive(FFieldChunk& Chunk, int32 Index)
{
	if (!Chunk.ActiveStamp.IsValidIndex(Index))
	{
		return;
	}

	// Stamped 2 for "next", so a cell already in this tick's list is not added
	// twice and the two sets never alias.
	if (Chunk.ActiveStamp[Index] == 2)
	{
		return;
	}

	Chunk.ActiveStamp[Index] = 2;
	Chunk.NextActive.Add(Index);
}

// ---------------------------------------------------------------------------
// Seeding
// ---------------------------------------------------------------------------

float UARPGSpreadSubsystem::AddExposure(FVector WorldPosition, float Radius, float Amount,
	FGameplayTag ElementTag)
{
	const int32 MediumIndex = FindMedium(ElementTag);
	if (MediumIndex == INDEX_NONE || Amount <= 0.f)
	{
		return 0.f;
	}

	const FMedium& Medium = Media[MediumIndex];
	if (Medium.FieldSlot == INDEX_NONE)
	{
		return 0.f;
	}

	const float CellSize = GetCellSize();
	const int32 CellRadius = FMath::Max(0, FMath::CeilToInt(Radius / CellSize));
	float Deposited = 0.f;

	for (int32 DY = -CellRadius; DY <= CellRadius; ++DY)
	{
		for (int32 DX = -CellRadius; DX <= CellRadius; ++DX)
		{
			const FVector Sample = WorldPosition + FVector(DX * CellSize, DY * CellSize, 0.f);
			const float Distance = FVector2D(Sample.X - WorldPosition.X, Sample.Y - WorldPosition.Y).Size();
			if (Radius > 0.f && Distance > Radius)
			{
				continue;
			}

			FIntPoint Coord;
			int32 Index = 0;
			if (!ResolveCell(Sample, Coord, Index))
			{
				continue;
			}

			FFieldChunk& Chunk = FindOrAddChunk(Coord);
			EnsureSlot(Chunk, Medium.FieldSlot, Coord);

			// Linear falloff to nothing at the rim, so a deposit has a soft edge
			// rather than a disc that lights uniformly and then spreads from a
			// perfectly circular front.
			const float Falloff = Radius > 0.f ? FMath::Max(0.f, 1.f - Distance / Radius) : 1.f;
			const float Share = Amount * Falloff;
			if (Share <= 0.f)
			{
				continue;
			}

			// Ground with no fuel cannot hold the medium at all. Refusing here
			// rather than letting it accumulate is what makes a river bed a
			// genuine firebreak instead of one that reads as alight.
			if (!Medium.Definition->bPermanent && Chunk.Fuel[Medium.FieldSlot][Index] <= 0.f)
			{
				continue;
			}

			Chunk.Intensity[Medium.FieldSlot][Index] += Share;

			// A direct seed banks in FULL -- transfer keep governs only
			// fire-to-fire hand-off, not what a spell puts in.
			Chunk.Energy[Medium.FieldSlot][Index] += Share;

			MarkActive(Chunk, Index);
			Deposited += Share;
		}
	}

	return Deposited;
}

void UARPGSpreadSubsystem::Extinguish(FVector WorldPosition, float Radius, FGameplayTag ElementTag)
{
	const int32 MediumIndex = FindMedium(ElementTag);
	if (MediumIndex == INDEX_NONE)
	{
		return;
	}

	const int32 Slot = Media[MediumIndex].FieldSlot;
	if (Slot == INDEX_NONE)
	{
		return;
	}

	const float CellSize = GetCellSize();
	const int32 CellRadius = FMath::Max(0, FMath::CeilToInt(Radius / CellSize));

	for (int32 DY = -CellRadius; DY <= CellRadius; ++DY)
	{
		for (int32 DX = -CellRadius; DX <= CellRadius; ++DX)
		{
			// A DISC, matching AddExposure and this function's own contract. The
			// square this used to walk quenched the corners too -- about a
			// quarter more ground than the caller asked for.
			if (Radius > 0.f && FMath::Square(DX * CellSize) + FMath::Square(DY * CellSize)
				> FMath::Square(Radius))
			{
				continue;
			}

			const FVector Sample = WorldPosition + FVector(DX * CellSize, DY * CellSize, 0.f);

			FIntPoint Coord;
			int32 Index = 0;
			if (!ResolveCell(Sample, Coord, Index))
			{
				continue;
			}

			FFieldChunk* Chunk = FindChunk(Coord);
			if (!Chunk || !Chunk->bSlotUsed[Slot])
			{
				continue;
			}

			Chunk->Intensity[Slot][Index] = 0.f;

			// The bank goes too. Leaving it would let a re-ignition here inherit
			// the reach of a fire that was deliberately put out.
			Chunk->Energy[Slot][Index] = 0.f;
		}
	}
}

void UARPGSpreadSubsystem::SetFieldFuel(FVector WorldPosition, float Radius, float FuelFraction,
	FGameplayTag ElementTag)
{
	const int32 MediumIndex = FindMedium(ElementTag);
	if (MediumIndex == INDEX_NONE)
	{
		return;
	}

	const FMedium& Medium = Media[MediumIndex];
	if (Medium.FieldSlot == INDEX_NONE)
	{
		return;
	}

	const float CellSize = GetCellSize();
	const int32 CellRadius = FMath::Max(0, FMath::CeilToInt(Radius / CellSize));
	const float Fuel = FMath::Clamp(FuelFraction, 0.f, 1.f) * Medium.Definition->FuelSeconds;

	for (int32 DY = -CellRadius; DY <= CellRadius; ++DY)
	{
		for (int32 DX = -CellRadius; DX <= CellRadius; ++DX)
		{
			// A DISC, as documented. Painting a firebreak used to square off its
			// corners, which is visible the moment a fire reaches one.
			if (Radius > 0.f && FMath::Square(DX * CellSize) + FMath::Square(DY * CellSize)
				> FMath::Square(Radius))
			{
				continue;
			}

			const FVector Sample = WorldPosition + FVector(DX * CellSize, DY * CellSize, 0.f);

			FIntPoint Coord;
			int32 Index = 0;
			if (!ResolveCell(Sample, Coord, Index))
			{
				continue;
			}

			FFieldChunk& Chunk = FindOrAddChunk(Coord);
			EnsureSlot(Chunk, Medium.FieldSlot, Coord);
			Chunk.Fuel[Medium.FieldSlot][Index] = Fuel;
		}
	}
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

float UARPGSpreadSubsystem::GetFieldIntensity(FVector WorldPosition, FGameplayTag ElementTag) const
{
	const int32 MediumIndex = FindMedium(ElementTag);
	if (MediumIndex == INDEX_NONE || Media[MediumIndex].FieldSlot == INDEX_NONE)
	{
		return 0.f;
	}

	FIntPoint Coord;
	int32 Index = 0;
	if (!ResolveCell(WorldPosition, Coord, Index))
	{
		return 0.f;
	}

	const FFieldChunk* Chunk = FindChunk(Coord);
	const int32 Slot = Media[MediumIndex].FieldSlot;

	return (Chunk && Chunk->bSlotUsed[Slot]) ? Chunk->Intensity[Slot][Index] : 0.f;
}

float UARPGSpreadSubsystem::GetScorchAt(FVector WorldPosition, FGameplayTag ElementTag) const
{
	const int32 MediumIndex = FindMedium(ElementTag);
	if (MediumIndex == INDEX_NONE || Media[MediumIndex].FieldSlot == INDEX_NONE)
	{
		return 0.f;
	}

	const UARPGSpreadDefinition* Definition = Media[MediumIndex].Definition;

	// Nothing that ignores fuel can be scorched by spending it.
	if (!Definition || Definition->bPermanent || Definition->FuelSeconds <= 0.f)
	{
		return 0.f;
	}

	FIntPoint Coord;
	int32 Index = 0;
	if (!ResolveCell(WorldPosition, Coord, Index))
	{
		return 0.f;
	}

	const FFieldChunk* Chunk = FindChunk(Coord);
	const int32 Slot = Media[MediumIndex].FieldSlot;

	// A chunk that has never been allocated has never burned.
	if (!Chunk || !Chunk->bSlotUsed[Slot])
	{
		return 0.f;
	}

	// AGAINST WHAT THE CELL STARTED WITH, not against a global maximum. A marsh
	// that only ever held a tenth of a grassland's fuel still reads fully scorched
	// once it has given that tenth up -- scorch is "how spent is this", and a
	// marsh burned out is as burned out as anything gets.
	//
	// Recomputed from the map rather than stored. The bake is static, so the
	// starting amount is a pure function of surface and jitter, and keeping a
	// second float per cell per medium to hold a number that cannot change would
	// be paying memory for arithmetic.
	float Started = Definition->FuelSeconds;

	if (FuelMap && FieldResolution > 0)
	{
		const int32 X = Index % FieldResolution;
		const int32 Y = Index / FieldResolution;

		const float NormalX = (X + 0.5f) / FieldResolution;
		const float NormalY = (Y + 0.5f) / FieldResolution;

		Started *= Definition->GetSurfaceFuel(FuelMap->GetSurfaceAt(Coord, NormalX, NormalY))
			* FuelMap->GetJitterAt(Coord, NormalX, NormalY);
	}

	// Ground that never had any fuel is not scorched, it is stone.
	if (Started <= KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}

	return FMath::Clamp(1.f - Chunk->Fuel[Slot][Index] / Started, 0.f, 1.f);
}

float UARPGSpreadSubsystem::GetFieldResidue(FVector WorldPosition, FGameplayTag ElementTag) const
{
	const int32 MediumIndex = FindMedium(ElementTag);
	if (MediumIndex == INDEX_NONE || Media[MediumIndex].FieldSlot == INDEX_NONE)
	{
		return 0.f;
	}

	FIntPoint Coord;
	int32 Index = 0;
	if (!ResolveCell(WorldPosition, Coord, Index))
	{
		return 0.f;
	}

	const FFieldChunk* Chunk = FindChunk(Coord);
	const int32 Slot = Media[MediumIndex].FieldSlot;

	return (Chunk && Chunk->bSlotUsed[Slot]) ? Chunk->Residue[Slot][Index] : 0.f;
}

float UARPGSpreadSubsystem::GetFieldFuel(FVector WorldPosition, FGameplayTag ElementTag) const
{
	const int32 MediumIndex = FindMedium(ElementTag);
	if (MediumIndex == INDEX_NONE || Media[MediumIndex].FieldSlot == INDEX_NONE)
	{
		return 0.f;
	}

	FIntPoint Coord;
	int32 Index = 0;
	if (!ResolveCell(WorldPosition, Coord, Index))
	{
		return 0.f;
	}

	const FFieldChunk* Chunk = FindChunk(Coord);
	const int32 Slot = Media[MediumIndex].FieldSlot;

	if (!Chunk || !Chunk->bSlotUsed[Slot])
	{
		// Unallocated means untouched, which is full fuel -- not empty. The
		// opposite would make an unvisited chunk fireproof.
		return Media[MediumIndex].Definition->FuelSeconds;
	}

	return Chunk->Fuel[Slot][Index];
}

float UARPGSpreadSubsystem::GetFieldEnergy(FVector WorldPosition, FGameplayTag ElementTag) const
{
	const int32 MediumIndex = FindMedium(ElementTag);
	if (MediumIndex == INDEX_NONE || Media[MediumIndex].FieldSlot == INDEX_NONE)
	{
		return 0.f;
	}

	FIntPoint Coord;
	int32 Index = 0;
	if (!ResolveCell(WorldPosition, Coord, Index))
	{
		return 0.f;
	}

	const FFieldChunk* Chunk = FindChunk(Coord);
	const int32 Slot = Media[MediumIndex].FieldSlot;

	return (Chunk && Chunk->bSlotUsed[Slot]) ? Chunk->Energy[Slot][Index] : 0.f;
}

bool UARPGSpreadSubsystem::IsBurning(FVector WorldPosition, FGameplayTag ElementTag) const
{
	const int32 MediumIndex = FindMedium(ElementTag);
	if (MediumIndex == INDEX_NONE)
	{
		return false;
	}

	return GetFieldIntensity(WorldPosition, ElementTag)
		>= Media[MediumIndex].Definition->IgnitionThreshold;
}

int32 UARPGSpreadSubsystem::GetBurningCellCount(FGameplayTag ElementTag) const
{
	const int32 MediumIndex = FindMedium(ElementTag);
	if (MediumIndex == INDEX_NONE || Media[MediumIndex].FieldSlot == INDEX_NONE)
	{
		return 0;
	}

	const int32 Slot = Media[MediumIndex].FieldSlot;
	const float Threshold = Media[MediumIndex].Definition->IgnitionThreshold;

	int32 Count = 0;
	for (const TPair<FIntPoint, FFieldChunk>& Pair : Chunks)
	{
		if (!Pair.Value.bSlotUsed[Slot])
		{
			continue;
		}

		for (const float Value : Pair.Value.Intensity[Slot])
		{
			if (Value >= Threshold)
			{
				++Count;
			}
		}
	}

	return Count;
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

float UARPGSpreadSubsystem::GetWindWeight(FVector2D StepDirection, float Bias) const
{
	if (WindStrength <= 0.f || Bias <= 0.f)
	{
		return 1.f;
	}

	const FVector2D Wind = WindDirection.GetSafeNormal();
	if (Wind.IsNearlyZero())
	{
		return 1.f;
	}

	// -1 upwind, +1 downwind, mapped so a full-bias medium in a full gale barely
	// travels upwind at all while an unbiased one is a symmetric disc.
	const float Alignment = FVector2D::DotProduct(StepDirection.GetSafeNormal(), Wind);
	return FMath::Max(0.f, 1.f + Bias * WindStrength * Alignment);
}

void UARPGSpreadSubsystem::StepSimulation(float DeltaTime)
{
	EnsureMedia();

	TickField(DeltaTime);
	TickFuelSources(DeltaTime);
	TickContactDamage(DeltaTime);

	// LAST, so it draws the field as it now is rather than as it was.
	TickMask();
}

void UARPGSpreadSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// The field is server state. A client simulating its own copy would diverge
	// immediately -- nothing here replicates -- and TickContactDamage would then
	// apply gameplay effects and consume invincibility frames from it.
	if (!HasAuthority())
	{
		return;
	}

	const float Interval = 1.f / FMath::Max(1.f, TickRate);

	TickAccumulator += DeltaTime;
	if (TickAccumulator < Interval)
	{
		return;
	}

	// The accumulated time, not the frame's: the simulation advances in real
	// seconds regardless of frame rate, so a fire does not spread faster on a
	// faster machine.
	StepSimulation(TickAccumulator);
	TickAccumulator = 0.f;
}

void UARPGSpreadSubsystem::EnsureScratch()
{
	// SIZED FOR EVERY SLOT, once. Growing one inside the parallel sweep would be
	// an allocation on a worker and, worse, a resize of a member array while
	// another worker holds a reference into it.
	const int32 CellCount = FieldResolution * FieldResolution;

	for (FChunkWork& Work : ChunkWork)
	{
		if (Work.DeltaIntensity.Num() != CellCount)
		{
			Work.DeltaIntensity.SetNumZeroed(CellCount);
			Work.DeltaEnergy.SetNumZeroed(CellCount);
		}
	}
}

void UARPGSpreadSubsystem::TickField(float DeltaTime)
{
	CrossDeposits.Reset();

	// FLATTENED FIRST, because a TMap cannot be indexed and the sweep needs to be.
	// Only the chunks with something alight: an inert chunk's step is a no-op, and
	// handing one to a worker costs more than skipping it here.
	Burning.Reset();

	for (TPair<FIntPoint, FFieldChunk>& Pair : Chunks)
	{
		if (Pair.Value.Active.Num() > 0)
		{
			Burning.Emplace(Pair.Key, &Pair.Value);
		}
	}

	if (Burning.Num() > ChunkWork.Num())
	{
		ChunkWork.SetNum(Burning.Num());
	}

	// After the slots exist, and never from inside the sweep.
	EnsureScratch();

	// EMBARRASSINGLY PARALLEL, and it was already written that way without anyone
	// intending it: a chunk step reads shared configuration, writes its own cells,
	// and posts anything crossing a boundary to a queue for afterwards. The only
	// two things it shared were the scratch list and that queue, and both are now
	// per-slot.
	//
	// ForceSingleThread below a handful of chunks, because the dispatch costs more
	// than the work when a single campfire is burning -- which is most of the time.
	ParallelFor(Burning.Num(),
		[this, DeltaTime](int32 Index)
		{
			ChunkWork[Index].Deposits.Reset();
			TickFieldChunk(Burning[Index].Key, *Burning[Index].Value, DeltaTime,
				ChunkWork[Index]);
		},
		/*bForceSingleThread=*/Burning.Num() < 4);

	// GATHERED IN ORDER, not as they finished. Two chunks depositing into the same
	// cell must land in the same sequence on every machine, or a listen server and
	// its client disagree about where a fire spread -- and thread completion order
	// is the least reproducible thing available.
	for (int32 Index = 0; Index < Burning.Num(); ++Index)
	{
		CrossDeposits.Append(ChunkWork[Index].Deposits);
	}

	ApplyCrossDeposits();

	// Swap the frontier in, and drop chunks that went completely inert. Freeing
	// them is what keeps a long session's memory proportional to what is alight
	// rather than to everywhere a fire has ever been.
	TArray<FIntPoint> Inert;

	for (TPair<FIntPoint, FFieldChunk>& Pair : Chunks)
	{
		FFieldChunk& Chunk = Pair.Value;

		for (const int32 Index : Chunk.Active)
		{
			if (Chunk.ActiveStamp.IsValidIndex(Index) && Chunk.ActiveStamp[Index] == 1)
			{
				Chunk.ActiveStamp[Index] = 0;
			}
		}

		Chunk.Active = MoveTemp(Chunk.NextActive);
		Chunk.NextActive.Reset();

		for (const int32 Index : Chunk.Active)
		{
			Chunk.ActiveStamp[Index] = 1;
		}

		if (Chunk.Active.Num() == 0 && Chunk.ResidueCells == 0)
		{
			Inert.Add(Pair.Key);
		}
	}

	for (const FIntPoint& Coord : Inert)
	{
		Chunks.Remove(Coord);
	}
}

void UARPGSpreadSubsystem::TickFieldChunk(FIntPoint Coord, FFieldChunk& Chunk, float DeltaTime,
	FChunkWork& Work)
{
	const int32 Resolution = FieldResolution;

	// Attrition runs BEFORE the media step, on last tick's active list, so a
	// cell doused this tick does not also get to spread this tick.
	TickAttrition(Coord, Chunk, DeltaTime);

	// Snapshotted: the pass writes into NextActive, and iterating a list being
	// appended to would visit this tick's newly lit cells as though they had
	// been burning all along. Into a reused member buffer rather than a fresh
	// TArray per chunk per tick.
	Work.Active.Reset(Chunk.Active.Num());
	Work.Active.Append(Chunk.Active);
	const TArray<int32>& Active = Work.Active;

	for (int32 MediumIndex = 0; MediumIndex < Media.Num(); ++MediumIndex)
	{
		const FMedium& Medium = Media[MediumIndex];
		const int32 Slot = Medium.FieldSlot;
		if (Slot == INDEX_NONE || !Chunk.bSlotUsed[Slot])
		{
			continue;
		}

		const UARPGSpreadDefinition* Definition = Medium.Definition;
		const float Threshold = Definition->IgnitionThreshold;
		const float Rate = Definition->SpreadRate;
		const float Decay = Definition->DecayRate;
		const float Bias = Definition->WindBias;
		const float FuelSeconds = FMath::Max(0.01f, Definition->FuelSeconds);
		const bool bPermanent = Definition->bPermanent;
		const bool bConserve = Definition->bConserveEnergy;

		TArray<float>& Intensity = Chunk.Intensity[Slot];
		TArray<float>& Fuel = Chunk.Fuel[Slot];
		TArray<float>& Residue = Chunk.Residue[Slot];
		TArray<float>& Energy = Chunk.Energy[Slot];

		// Deltas accumulate across the whole pass, so every sender contributes
		// to a cell before it decides whether it caught -- order-independent.
		//
		// Flat arrays indexed by cell, reused across chunks and media, with a
		// list of the cells actually written so the reset costs the number of
		// touched cells rather than the size of the grid. This was two TMaps
		// constructed and destroyed inside this loop -- so per chunk, per medium,
		// per simulation tick.
		TArray<float>& DeltaIntensity = Work.DeltaIntensity;
		TArray<float>& DeltaEnergy = Work.DeltaEnergy;
		ScratchTouchedIntensity.Reset();
		ScratchTouchedEnergy.Reset();

		for (const int32 Index : Active)
		{
			const float Value = Intensity[Index];
			if (Value <= ColdEpsilon)
			{
				// Cold, and NOT necessarily by cooling: attrition can drive a cell
				// straight to zero without it ever passing through the branch
				// below. Dropping the bank here as well is what stops rain-
				// quenched ground re-igniting at the reach it had before -- the
				// cell was put out, so its unspent energy is gone with it.
				Energy[Index] = 0.f;
				continue;
			}

			const bool bBurning = Value >= Threshold && (bPermanent || Fuel[Index] > 0.f);

			if (!bBurning)
			{
				// Either still smouldering below the threshold, or spent. Spent
				// ground cools FASTER, so a burnt-out front visibly goes out
				// rather than lingering warm.
				const float Loss = (!bPermanent && Fuel[Index] <= 0.f) ? Decay * 3.f : Decay;
				const float Next = FMath::Max(0.f, Value - Loss * DeltaTime);
				Intensity[Index] = Next;

				if (Next > ColdEpsilon)
				{
					MarkNextActive(Chunk, Index);
				}
				else
				{
					// Fully cold: drop the unspent bank, so a much later and
					// unrelated re-ignition here starts from scratch rather than
					// inheriting an old fire's reach.
					Energy[Index] = 0.f;
				}
				continue;
			}

			if (!bPermanent)
			{
				Fuel[Index] = FMath::Max(0.f, Fuel[Index] - DeltaTime);
			}

			// Counted on the transition only, so the total stays exact rather
			// than being a one-way flag that pins the chunk in memory forever.
			const float PreviousResidue = Residue[Index];
			Residue[Index] = FMath::Min(1.f, PreviousResidue + DeltaTime / FuelSeconds);
			if (PreviousResidue <= ColdEpsilon && Residue[Index] > ColdEpsilon)
			{
				++Chunk.ResidueCells;
			}

			// Saturates rather than growing without bound, so intensity stays
			// meaningful as a normalised value for the visual mask.
			Intensity[Index] = FMath::Min(Value + Rate * DeltaTime, Threshold * 2.f);
			MarkNextActive(Chunk, Index);

			// Combustion: burning fuel feeds this cell's own bank. Tuned below
			// the cost of igniting a fresh cell, so open grass alone is always a
			// losing proposition and a front lives on what was put in.
			if (bConserve && (bPermanent || Fuel[Index] > 0.f))
			{
				Energy[Index] += Definition->FuelEnergyYield * DeltaTime;
			}

			// -1 means unbudgeted: a non-conserving medium pushes at its rate
			// unconditionally.
			float Budget = bConserve ? Energy[Index] : -1.f;

			const int32 CellX = Index % Resolution;
			const int32 CellY = Index / Resolution;

			for (int32 DY = -1; DY <= 1 && Budget != 0.f; ++DY)
			{
				for (int32 DX = -1; DX <= 1; ++DX)
				{
					if (DX == 0 && DY == 0)
					{
						continue;
					}

					const bool bOrtho = (DX == 0 || DY == 0);
					const float StepWeight = bOrtho ? OrthoWeight : DiagonalWeight;
					const float InvLength = bOrtho ? 1.f : InvSqrt2;

					const float Weight = GetWindWeight(
						FVector2D(DX * InvLength, DY * InvLength), Bias);

					float Amount = Rate * StepWeight * Weight * DeltaTime;
					if (Budget >= 0.f)
					{
						Amount = FMath::Min(Amount, Budget);
					}
					if (Amount <= 0.f)
					{
						continue;
					}

					const int32 NX = CellX + DX;
					const int32 NY = CellY + DY;

					if (NX >= 0 && NX < Resolution && NY >= 0 && NY < Resolution)
					{
						const int32 NeighbourIndex = NY * Resolution + NX;

						if (Intensity[NeighbourIndex] >= Threshold)
						{
							// Already alight, so it cannot use exposure -- but DO
							// conduct bank downhill into it. See ConductionScale:
							// without this a strongly seeded patch strands its
							// energy behind its own front.
							if (bConserve && Budget > 0.f && Fuel[NeighbourIndex] > 0.f)
							{
								float Conduct = FMath::Min(Amount * ConductionScale,
									(Budget - Energy[NeighbourIndex]) * 0.5f);
								Conduct = FMath::Min(Conduct, Budget);

								if (Conduct > 0.f)
								{
									if (DeltaEnergy[NeighbourIndex] == 0.f)
									{
										ScratchTouchedEnergy.Add(NeighbourIndex);
									}
									DeltaEnergy[NeighbourIndex] += Conduct;
									Budget -= Conduct;
								}
							}
							continue;
						}

						if (DeltaIntensity[NeighbourIndex] == 0.f)
						{
							ScratchTouchedIntensity.Add(NeighbourIndex);
						}
						DeltaIntensity[NeighbourIndex] += Amount;

						// The receiver banks only TransferKeep of what it was
						// handed; the sender paid all of it. That gap is the
						// front's per-hop range decay.
						if (DeltaEnergy[NeighbourIndex] == 0.f)
						{
							ScratchTouchedEnergy.Add(NeighbourIndex);
						}
						DeltaEnergy[NeighbourIndex] += Amount * Definition->TransferKeep;
					}
					else
					{
						// Across a chunk edge. Deferred and resolved in world
						// space, so a fire never stops at a chunk border.
						const float CellSize = GetCellSize();
						const FVector World(
							Coord.X * ChunkSize + (CellX + DX + 0.5f) * CellSize,
							Coord.Y * ChunkSize + (CellY + DY + 0.5f) * CellSize,
							0.f);

						FCrossDeposit Deposit;
						if (ResolveCell(World, Deposit.Coord, Deposit.Index))
						{
							Deposit.Medium = MediumIndex;
							Deposit.Amount = Amount;
							Deposit.Energy = Amount * Definition->TransferKeep;
							Work.Deposits.Add(Deposit);
						}
					}

					if (Budget >= 0.f)
					{
						Budget -= Amount;
						if (Budget <= 0.f)
						{
							Budget = 0.f;
							break;
						}
					}
				}
			}

			if (bConserve)
			{
				Energy[Index] = FMath::Max(0.f, Budget);
			}
		}

		// Applied after the pass, so every contribution to a cell is counted
		// before it decides whether it caught.
		for (const int32 Index : ScratchTouchedIntensity)
		{
			const float Delta = DeltaIntensity[Index];
			DeltaIntensity[Index] = 0.f; // reset as we go; the buffer is shared

			// Ground with no fuel cannot hold the medium -- a road, a river, or
			// somewhere the front already burnt out. Without this a spent cell
			// keeps accumulating from burning neighbours: it never spreads
			// onward, but it saturates, so a firebreak reads as fully alight and
			// deals contact damage to anyone standing on it.
			if (!bPermanent && Fuel[Index] <= 0.f)
			{
				continue;
			}

			Intensity[Index] = FMath::Min(Intensity[Index] + Delta, Threshold * 2.f);
			MarkNextActive(Chunk, Index);
		}

		for (const int32 Index : ScratchTouchedEnergy)
		{
			const float Delta = DeltaEnergy[Index];
			DeltaEnergy[Index] = 0.f;

			if (!bPermanent && Fuel[Index] <= 0.f)
			{
				continue;
			}
			Energy[Index] += Delta;
		}
	}
}

void UARPGSpreadSubsystem::ApplyCrossDeposits()
{
	for (const FCrossDeposit& Deposit : CrossDeposits)
	{
		const FMedium& Medium = Media[Deposit.Medium];
		const int32 Slot = Medium.FieldSlot;
		if (Slot == INDEX_NONE)
		{
			continue;
		}

		FFieldChunk& Chunk = FindOrAddChunk(Deposit.Coord);
		EnsureSlot(Chunk, Slot, Deposit.Coord);

		if (!Medium.Definition->bPermanent && Chunk.Fuel[Slot][Deposit.Index] <= 0.f)
		{
			continue;
		}

		const float Threshold = Medium.Definition->IgnitionThreshold;
		Chunk.Intensity[Slot][Deposit.Index] =
			FMath::Min(Chunk.Intensity[Slot][Deposit.Index] + Deposit.Amount, Threshold * 2.f);
		Chunk.Energy[Slot][Deposit.Index] += Deposit.Energy;

		MarkNextActive(Chunk, Deposit.Index);
	}

	CrossDeposits.Reset();
}

void UARPGSpreadSubsystem::TickAttrition(FIntPoint Coord, FFieldChunk& Chunk, float DeltaTime)
{
	// Every pair of co-located media destroys each other at the rates the shared
	// combination table authored. Rain quenching a grass fire is not a special
	// case here: it is the same row a water jet meeting a fireball reads.
	for (int32 A = 0; A < Media.Num(); ++A)
	{
		const int32 SlotA = Media[A].FieldSlot;
		if (SlotA == INDEX_NONE || !Chunk.bSlotUsed[SlotA])
		{
			continue;
		}

		for (int32 B = 0; B < Media.Num(); ++B)
		{
			const int32 SlotB = Media[B].FieldSlot;
			if (A == B || SlotB == INDEX_NONE || !Chunk.bSlotUsed[SlotB])
			{
				continue;
			}

			const float Rate = AttritionRates[A][B];
			if (Rate <= 0.f)
			{
				continue;
			}

			for (const int32 Index : Chunk.Active)
			{
				const float Opposing = Chunk.Intensity[SlotB][Index];
				if (Opposing <= ColdEpsilon)
				{
					continue;
				}

				const float Loss = Rate * Opposing * DeltaTime;

				Chunk.Intensity[SlotA][Index] =
					FMath::Max(0.f, Chunk.Intensity[SlotA][Index] - Loss);

				// The bank goes with it. A quenched cell that kept its energy
				// would re-ignite at full reach the moment the rain stopped.
				Chunk.Energy[SlotA][Index] =
					FMath::Max(0.f, Chunk.Energy[SlotA][Index] - Loss);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Targets and contact damage
// ---------------------------------------------------------------------------

void UARPGSpreadSubsystem::TickMask()
{
	UWorld* World = GetWorld();

	if (!World || !MaskMedium.IsValid() || MaskResolution < 16)
	{
		return;
	}

	// WHERE THE PLAYER IS, because a mask covering the whole world would be a
	// texture nobody can afford and mostly black. A window that follows the
	// viewer is the same trick the significance pass uses, for the same reason.
	FVector Centre = FVector::ZeroVector;
	bool bFound = false;

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		if (const APlayerController* Controller = It->Get())
		{
			if (const AActor* View = Controller->GetPawn()
				? Cast<AActor>(Controller->GetPawn())
				: Controller->GetViewTarget())
			{
				Centre = View->GetActorLocation();
				bFound = true;
				break;
			}
		}
	}

	// A dedicated server has nobody to draw for, and neither does a test.
	if (!bFound)
	{
		return;
	}

	const float Cell = GetCellSize();
	const float Extent = Cell * MaskResolution;
	const FVector2D Origin(Centre.X - Extent * 0.5f, Centre.Y - Extent * 0.5f);

	const int32 Pixels = MaskResolution * MaskResolution;

	if (!FieldMask || FieldMask->GetSizeX() != MaskResolution)
	{
		FieldMask = UTexture2D::CreateTransient(MaskResolution, MaskResolution, PF_B8G8R8A8);

		if (!FieldMask)
		{
			return;
		}

		// BILINEAR IS THE WHOLE POINT, not a nicety. Read per cell, a fire's edge
		// steps along cell boundaries and the same fire looks different depending
		// on where the viewer is standing relative to one -- which reads as the
		// simulation being unreliable rather than as aliasing. A filtered texture
		// has no cells to stand on. Clamped, because the window ends and wrapping
		// would put the far side of the fire under your feet.
		FieldMask->Filter = TF_Bilinear;
		FieldMask->AddressX = TA_Clamp;
		FieldMask->AddressY = TA_Clamp;
		FieldMask->SRGB = false;
		FieldMask->NeverStream = true;
		FieldMask->AddToRoot();
		FieldMask->UpdateResource();
	}

	MaskPixels.SetNumUninitialized(Pixels);

	for (int32 Y = 0; Y < MaskResolution; ++Y)
	{
		for (int32 X = 0; X < MaskResolution; ++X)
		{
			const FVector At(
				Origin.X + (X + 0.5f) * Cell,
				Origin.Y + (Y + 0.5f) * Cell,
				Centre.Z);

			const float Intensity = FMath::Clamp(GetFieldIntensity(At, MaskMedium), 0.f, 1.f);
			const float Scorch = GetScorchAt(At, MaskMedium);

			// R alight, G spent. Scorch is the one that persists: a fire passes
			// and the ground stays black, which is the entire reason the fuel map
			// is the right home for grass rather than a component per tuft.
			MaskPixels[Y * MaskResolution + X] = FColor(
				static_cast<uint8>(Intensity * 255.f),
				static_cast<uint8>(Scorch * 255.f),
				0, 255);
		}
	}

	if (FTexture2DMipMap* Mip = &FieldMask->GetPlatformData()->Mips[0])
	{
		void* Data = Mip->BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Data, MaskPixels.GetData(), Pixels * sizeof(FColor));
		Mip->BulkData.Unlock();
		FieldMask->UpdateResource();
	}

	// AND WHERE IT GOES. Four numbers is exactly what a parameter collection is
	// for, and exactly what it can hold -- the field itself was never going to fit
	// in one, which is why the plan's "MPC mask" stayed a forward declaration.
	if (MaskParameters)
	{
		UKismetMaterialLibrary::SetScalarParameterValue(
			World, MaskParameters, TEXT("MaskOriginX"), Origin.X);
		UKismetMaterialLibrary::SetScalarParameterValue(
			World, MaskParameters, TEXT("MaskOriginY"), Origin.Y);
		UKismetMaterialLibrary::SetScalarParameterValue(
			World, MaskParameters, TEXT("MaskExtent"), Extent);
	}
}

void UARPGSpreadSubsystem::RegisterFuel(UARPGFuelComponent* Fuel)
{
	if (Fuel)
	{
		FuelSources.AddUnique(Fuel);
	}
}

void UARPGSpreadSubsystem::UnregisterFuel(UARPGFuelComponent* Fuel)
{
	FuelSources.Remove(Fuel);
}

void UARPGSpreadSubsystem::TickFuelSources(float DeltaTime)
{
	for (int32 Index = FuelSources.Num() - 1; Index >= 0; --Index)
	{
		UARPGFuelComponent* Fuel = FuelSources[Index].Get();

		if (!Fuel || !Fuel->GetOwner())
		{
			FuelSources.RemoveAt(Index);
			continue;
		}

		// SPENT ONES STAY REGISTERED. An object with nothing left to give is still
		// a thing the world contains, and it can be repaired -- a barricade rebuilt
		// mid-fight goes straight back to burning without anyone re-registering it.
		// Burn answers zero for them, which costs one comparison.
		const FVector Where = Fuel->GetOwner()->GetActorLocation();
		const float Intensity = GetFieldIntensity(Where, Fuel->MediumTag);

		const float Output = Fuel->Burn(DeltaTime, Intensity);

		if (Output <= 0.f)
		{
			continue;
		}

		// AND IT FEEDS THE FIRE BACK. This is what makes a woodpile worth setting
		// alight rather than merely flammable: the object sustains a fire the
		// ground around it could not, so where the burnable things are is a fact
		// about the map rather than a detail.
		//
		// Through AddExposure like anything else, so an object's contribution is
		// bounded by the same fuel the ground has and cannot light bare stone.
		AddExposure(Where, Fuel->Radius, Output * DeltaTime, Fuel->MediumTag);
	}
}

void UARPGSpreadSubsystem::RegisterTarget(AActor* Actor)
{
	if (Actor)
	{
		Targets.AddUnique(Actor);
	}
}

void UARPGSpreadSubsystem::UnregisterTarget(AActor* Actor)
{
	Targets.RemoveSingleSwap(Actor);
}

void UARPGSpreadSubsystem::TickContactDamage(float DeltaTime)
{
	// The cheap way round. There are few characters and potentially thousands of
	// hot cells, so this walks the targets and samples the field under each,
	// rather than the field spawning volumes to find the targets.
	for (int32 Index = Targets.Num() - 1; Index >= 0; --Index)
	{
		AActor* Target = Targets[Index].Get();
		if (!Target)
		{
			Targets.RemoveAt(Index);
			continue;
		}

		UARPGHurtboxComponent* Hurtbox = UARPGHurtboxComponent::FindFor(Target);
		if (!Hurtbox)
		{
			continue;
		}

		// Resolved once per target rather than once per target per medium: the
		// chunk lookup and the cell solve do not depend on which medium is being
		// asked about.
		const FVector TargetLocation = Target->GetActorLocation();

		FIntPoint Coord;
		int32 CellIndex = 0;
		if (!ResolveCell(TargetLocation, Coord, CellIndex))
		{
			continue;
		}

		const FFieldChunk* Chunk = FindChunk(Coord);
		if (!Chunk)
		{
			continue; // nothing has ever burned here
		}

		for (const FMedium& Medium : Media)
		{
			const UARPGSpreadDefinition* Definition = Medium.Definition;
			if (Definition->ContactDamagePerSecond <= 0.f || Medium.FieldSlot == INDEX_NONE)
			{
				continue;
			}

			// Inlined from IsBurning, which would otherwise redo FindMedium's
			// linear scan and the cell solve for every target and every medium.
			if (!Chunk->bSlotUsed[Medium.FieldSlot]
				|| Chunk->Intensity[Medium.FieldSlot][CellIndex] < Definition->IgnitionThreshold)
			{
				continue;
			}

			// Through the hurtbox, so i-frames apply and dodging through fire
			// works exactly as dodging through a sword does.
			if (!Hurtbox->TryConsumeHit())
			{
				continue;
			}

			UAbilitySystemComponent* TargetASC = Hurtbox->GetAbilitySystemComponent();
			if (!TargetASC || !Medium.Definition->Element)
			{
				continue;
			}

			FGameplayEffectContextHandle ContextHandle = TargetASC->MakeEffectContext();
			ContextHandle.AddSourceObject(this);

			if (FARPGGameplayEffectContext* Context =
					FARPGGameplayEffectContext::ExtractFrom(ContextHandle))
			{
				Context->DamageType = Definition->Element->DamageType;
				Context->MagicElementTag = Medium.ElementTag;

				// Standing in a fire cannot be parried. There is no blow to read.
				Context->bUnblockable = true;
			}

			const FGameplayEffectSpecHandle Spec = TargetASC->MakeOutgoingSpec(
				UARPGDamageGameplayEffect::StaticClass(), 1.f, ContextHandle);

			if (Spec.IsValid())
			{
				Spec.Data->SetSetByCallerMagnitude(TAG_Data_Damage,
					Definition->ContactDamagePerSecond * DeltaTime);
				TargetASC->ApplyGameplayEffectSpecToSelf(*Spec.Data);
			}

			// The element's own status too, so standing in fire sets you alight
			// through exactly the path a fireball would.
			if (Definition->Element->OnHitEffect)
			{
				const FGameplayEffectSpecHandle StatusSpec = TargetASC->MakeOutgoingSpec(
					Definition->Element->OnHitEffect, 1.f, ContextHandle);
				if (StatusSpec.IsValid())
				{
					TargetASC->ApplyGameplayEffectSpecToSelf(*StatusSpec.Data);
				}
			}
		}
	}
}
