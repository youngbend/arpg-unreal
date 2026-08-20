// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGElementalReactionSubsystem.h"
#include "ARPGWorldAuthority.h"
#include "ARPGConductionSubsystem.h"
#include "ARPGDischargeContext.h"
#include "ARPGDischargeEffect.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGFluidSurfaceSubsystem.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicElement.h"
#include "ARPGWorld.h"
#include "ARPGWorldSettings.h"
#include "Engine/World.h"

void UARPGElementalReactionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Project settings fill in anything not already assigned, so a real session
	// has a combination table instead of resolving every meeting to nothing.
	if (!CombinationTable)
	{
		CombinationTable = UARPGWorldSettings::Get().CombinationTable.LoadSynchronous();
	}

	VolumesMetHandle = UARPGElementalVolumeComponent::OnVolumesMet.AddUObject(
		this, &UARPGElementalReactionSubsystem::HandleVolumesMet);
}

void UARPGElementalReactionSubsystem::Deinitialize()
{
	UARPGElementalVolumeComponent::OnVolumesMet.Remove(VolumesMetHandle);
	Super::Deinitialize();
}

bool UARPGElementalReactionSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

bool UARPGElementalReactionSubsystem::HasAuthority() const
{
	return ARPGWorld::WorldHasAuthority(GetWorld());
}

void UARPGElementalReactionSubsystem::HandleVolumesMet(UARPGElementalVolumeComponent* A,
	UARPGElementalVolumeComponent* B)
{
	// OnVolumesMet is process-wide, so a PIE session running a server and a
	// client world would otherwise have each subsystem resolving the other's
	// collisions. See the delegate's own comment.
	if (!A || A->GetWorld() != GetWorld())
	{
		return;
	}

	Resolve(A, B);
}

void UARPGElementalReactionSubsystem::Resolve(UARPGElementalVolumeComponent* A,
	UARPGElementalVolumeComponent* B)
{
	if (!A || !B)
	{
		return;
	}

	// Server only. A reaction spends both volumes' energy, spawns a product and
	// deals damage -- a client resolving its own copy would double-count every
	// collision it also receives from the server.
	if (!HasAuthority())
	{
		return;
	}

	UARPGMagicElement* ElementA = A->Element;
	UARPGMagicElement* ElementB = B->Element;
	if (!ElementA || !ElementB)
	{
		return; // a volume with no element is inert
	}

	// Two fireballs should merge or pass through each other, not annihilate.
	if (ElementA->ElementTag == ElementB->ElementTag)
	{
		return;
	}

	const float EnergyA = A->GetEnergy();
	const float EnergyB = B->GetEnergy();
	if (EnergyA <= 0.f || EnergyB <= 0.f)
	{
		return;
	}

	// HAS IT ACTUALLY REACHED THE WATER? A body of fluid is a tall box so a spell
	// arriving above it still enters, which means the overlap fires while the
	// projectile is still metres in the air -- and it detonated there, snapping
	// its splash down to a surface it had not touched.
	//
	// Nothing is lost by refusing: the reservoir polls its overlaps every tick
	// and resolves the pair the moment it does arrive.
	if (A->bReservoir != B->bReservoir)
	{
		const UARPGElementalVolumeComponent* Reservoir = A->bReservoir ? A : B;
		const UARPGElementalVolumeComponent* Incoming = A->bReservoir ? B : A;

		float Reach = 0.f;
		if (const UPrimitiveComponent* Shape = Incoming->OverlapSource)
		{
			Reach = FMath::Max(0.f, Incoming->GetVolumeLocation().Z
				- (Shape->Bounds.Origin.Z - Shape->Bounds.BoxExtent.Z));
		}

		if (!Reservoir->ContainsPoint(Incoming->GetVolumeLocation(), Reach))
		{
			return;
		}
	}

	// --- The whole model. See the class comment. ---------------------------
	const float Reacted = FMath::Min(EnergyA, EnergyB);
	const float Absorption = FMath::Max(A->Absorption, B->Absorption);
	const float Magnitude = Reacted * (1.f - Absorption);

	FGameplayTagContainer Pair;
	Pair.AddTag(ElementA->ElementTag);
	Pair.AddTag(ElementB->ElementTag);

	// The relationship, not just its result: rates and amplification efficiency
	// are authored on the same row, so a collision and the rain read identical
	// numbers.
	UARPGMagicCombinationEntry* Entry = CombinationTable
		? CombinationTable->ResolveEntry(Pair, EARPGCombinationScope::Collision)
		: nullptr;

	UARPGMagicElement* Product = Entry ? Entry->Result : nullptr;

	if (!Product && !bNeutraliseWithoutProduct)
	{
		return;
	}

	// --- Solidifying -------------------------------------------------------
	// Asked FIRST because it is the narrower question. An ice shard meeting a
	// water JET is a collision that trades energy; the same shard meeting a
	// PUDDLE freezes part of its surface into something you can stand on. Only a
	// Surface-scope row says the latter, and the fluid subsystem hands the pair
	// straight back when neither side is actually a body of fluid -- so this
	// narrows the case without ever swallowing the general one.
	//
	// Structurally identical to the conduction hand-off below, and for the same
	// reason: a mode that is not an energy trade belongs to the system that owns
	// that behaviour, not to this solver.
	if (UARPGFluidSurfaceSubsystem* Fluids = GetWorld()->GetSubsystem<UARPGFluidSurfaceSubsystem>())
	{
		if (Fluids->TrySolidify(A, B))
		{
			return;
		}
	}

	// --- Conduction --------------------------------------------------------
	// The row says this charge TRAVELS THROUGH the other side rather than
	// reacting with it. Handed off whole: the charge is spent entering the
	// medium, and the medium is a carrier, so neither trades energy here.
	if (Entry && Entry->GetConductedElement().IsValid())
	{
		const bool bAIsCharge = Entry->GetConductedElement() == ElementA->ElementTag;
		UARPGElementalVolumeComponent* Charge = bAIsCharge ? A : B;
		UARPGElementalVolumeComponent* Medium = bAIsCharge ? B : A;

		// NOT THROUGH THE ICE. A floe floating on a body of water roofs over what
		// is beneath it, and the water's own collider knows nothing about that --
		// so a bolt that struck the ice would enter the water underneath and
		// conduct from there. What it hit was the ice.
		//
		// Asked at the CHARGE's own position, because that is where the strike
		// landed. And asked for ANY medium, not only a reservoir: a floe forms on
		// whatever was frozen, and a pool is only a reservoir once it has gathered
		// past a threshold -- so gating on that meant ice on an ordinary puddle
		// roofed nothing at all, which is the common case rather than the rare
		// one. The query walks the live solids and is free when there are none.
		bool bRoofed = false;
		if (const UARPGFluidSurfaceSubsystem* Fluids =
				GetWorld()->GetSubsystem<UARPGFluidSurfaceSubsystem>())
		{
			bRoofed = Fluids->IsCoveredBySolid(Charge->GetVolumeLocation());
		}

		if (!bRoofed && Medium->Conductivity > 0.f)
		{
			if (UARPGConductionSubsystem* Conduction =
					GetWorld()->GetSubsystem<UARPGConductionSubsystem>())
			{
				Conduction->Conduct(bAIsCharge ? ElementA : ElementB, Medium,
					Charge->GetEnergy(), Charge->SourceActor);

				// Spent delivering itself into the medium.
				Charge->Consume(Charge->GetEnergy(), nullptr);
				return;
			}
		}
		// No conduction subsystem, or this medium does not carry -- fall through
		// and let the pair simply neutralise.
	}

	// --- Amplification -----------------------------------------------------
	// The recipe resolved to one of the reactants, so this is one element
	// feeding the other rather than the two making a third. The survivor eats
	// the whole of the loser: there is no partial exchange and no product burst,
	// because the product IS the survivor.
	if (Entry && Entry->IsAmplifying())
	{
		const bool bAmplifiesA = Entry->GetAmplifiedElement() == ElementA->ElementTag;
		UARPGElementalVolumeComponent* Winner = bAmplifiesA ? A : B;
		UARPGElementalVolumeComponent* Eaten = bAmplifiesA ? B : A;

		const float Gained = Eaten->GetEnergy() * Entry->AmplificationEfficiency;

		// Consume first: a reservoir shrugs this off, so an air current large
		// enough to be authored as one feeds a fireball forever. That is a
		// deliberate authoring choice rather than a bug.
		Eaten->Consume(Eaten->GetEnergy(), Product);
		Winner->Amplify(Gained, Product);

		LastProduct = Product;
		return;
	}

	// --- Contact point -----------------------------------------------------
	// A reservoir's own origin is not a meaningful position to average against:
	// a river's origin is the centre of a huge box, tens of metres from wherever
	// a fireball actually touched its edge. The midpoint is only a good
	// approximation for two comparably-sized volumes, so when exactly one side is
	// a reservoir, anchor on the other.
	FVector Contact;
	if (A->bReservoir != B->bReservoir)
	{
		const UARPGElementalVolumeComponent* Reservoir = A->bReservoir ? A : B;
		const UARPGElementalVolumeComponent* Incoming = A->bReservoir ? B : A;

		Contact = Incoming->GetVolumeLocation();

		// ...and put it ON THE WATERLINE. The surface sits inside a much taller
		// box, so the projectile's own centre can be half a metre off the surface
		// it visibly hit. Splashing where the water actually is costs one query
		// and is the whole difference between a hiss of steam on the surface and
		// one hanging over it.
		Contact.Z = Reservoir->GetSurfaceHeightAt(Contact);
	}
	else
	{
		Contact = (A->GetVolumeLocation() + B->GetVolumeLocation()) * 0.5f;
	}

	// Outward from the meeting, along the line between the two.
	FVector Direction = (B->GetVolumeLocation() - A->GetVolumeLocation()).GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		Direction = FVector::UpVector;
	}

	LastContactPoint = Contact;
	LastProduct = Product;

	// --- Exchange ----------------------------------------------------------
	// Consumption rates make a relationship lopsided: water quenches fire far
	// faster than fire boils off water, which is what makes a water jet punch
	// through a fireball rather than trade evenly with it.
	const float RateA = Entry ? Entry->GetConsumptionRate(ElementA->ElementTag) : 1.f;
	const float RateB = Entry ? Entry->GetConsumptionRate(ElementB->ElementTag) : 1.f;

	// Attribution is taken before consuming, because a fully spent volume may
	// destroy its owner from inside Consume.
	AActor* SourceActor = A->SourceActor ? A->SourceActor.Get() : B->SourceActor.Get();

	// WHERE, for anything that cares. A puddle does not -- a liquid loses ground
	// uniformly -- but a slab of ice melts at the point the fireball struck it,
	// and OnElementalReaction has no room to say so. Left here rather than
	// threaded through the hook's signature, which every projectile would then
	// carry for the sake of one case.
	UARPGFluidSurfaceSubsystem* Fluids = GetWorld()->GetSubsystem<UARPGFluidSurfaceSubsystem>();

	if (Fluids)
	{
		Fluids->NoteReactionContact(A, B, Contact);

		// And an empty ledger, so anything a consumed body hands back below is
		// attributable to THIS reaction when the product is spawned.
		Fluids->OpenReactionLedger();
	}

	A->Consume(Reacted * RateA, Product);
	B->Consume(Reacted * RateB, Product);

	if (Product && Magnitude >= MinMagnitude)
	{
		// A MELTED SLAB HAS ALREADY LEFT ITS FLUID at this very point, so the
		// product must not leave it again -- fire + ice -> water is one body of
		// water, not two. With no body consumed there is nothing else accounting
		// for it and the product's deposit is the only one there will be.
		const bool bProductDeposits = !Fluids || !Fluids->WasFluidReturned(Product->ElementTag);

		SpawnProduct(Product, Contact, Direction, Magnitude, SourceActor, bProductDeposits);
	}
}

void UARPGElementalReactionSubsystem::SpawnProduct(UARPGMagicElement* Product,
	const FVector& Contact, const FVector& Direction, float Magnitude, AActor* SourceActor,
	bool bDeposits)
{
	UWorld* World = GetWorld();
	if (!World || !Product)
	{
		return;
	}

	TSubclassOf<AActor> EffectClass = Product->ResolveDischargeEffect(EARPGDischargeType::Collision);
	if (!EffectClass)
	{
		return; // already warned by the element
	}

	// Authored exactly like any other spell effect. A reaction product needs no
	// machinery of its own -- that is the whole reason Collision is a discharge
	// type rather than a bespoke path.
	FARPGDischargeContext Context;
	Context.DischargeType = EARPGDischargeType::Collision;
	Context.Caster = SourceActor;
	Context.PrimaryElement = Product;
	Context.Origin = Contact;
	Context.Direction = Direction;
	Context.ComputedDamage = Magnitude * DamageScale;
	Context.ComputedPoiseDamage = Product->BasePoiseDamage;

	// Normalised against a reference magnitude rather than a charge, since a
	// reaction has no caster holding a button -- but expressed in the same
	// window units so effects read it identically to a cast spell's.
	Context.MinPower = 0.f;
	Context.MaxPower = 1.f;
	Context.PowerFraction = FMath::Clamp(Magnitude / PowerReference, 0.f, 1.f);

	const FTransform SpawnTransform(Direction.Rotation(), Contact);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* Spawned = World->SpawnActorDeferred<AActor>(
		EffectClass, SpawnTransform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);

	if (!Spawned)
	{
		return;
	}

	if (AARPGDischargeEffect* Effect = Cast<AARPGDischargeEffect>(Spawned))
	{
		Effect->InitializeFromContext(Context);

		// AFTER the effect has sized itself, because sizing is where the deposit
		// radius comes from -- an effect derives what it wets from what it covers,
		// and the one thing that can know this particular puddle is already spoken
		// for is the caller.
		if (!bDeposits)
		{
			Effect->DepositRadius = 0.f;
		}
	}

	Spawned->FinishSpawning(SpawnTransform);

	UE_LOG(LogARPGWorld, Verbose, TEXT("Reaction produced '%s' at %s (magnitude %.1f)"),
		*Product->ElementTag.ToString(), *Contact.ToCompactString(), Magnitude);
}
