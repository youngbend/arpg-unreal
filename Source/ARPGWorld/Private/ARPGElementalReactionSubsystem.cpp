// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGElementalReactionSubsystem.h"
#include "ARPGConductionSubsystem.h"
#include "ARPGDischargeContext.h"
#include "ARPGDischargeEffect.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicElement.h"
#include "ARPGWorld.h"
#include "Engine/World.h"

void UARPGElementalReactionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

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

	// --- Conduction --------------------------------------------------------
	// The row says this charge TRAVELS THROUGH the other side rather than
	// reacting with it. Handed off whole: the charge is spent entering the
	// medium, and the medium is a carrier, so neither trades energy here.
	if (Entry && Entry->GetConductedElement().IsValid())
	{
		const bool bAIsCharge = Entry->GetConductedElement() == ElementA->ElementTag;
		UARPGElementalVolumeComponent* Charge = bAIsCharge ? A : B;
		UARPGElementalVolumeComponent* Medium = bAIsCharge ? B : A;

		if (Medium->Conductivity > 0.f)
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

	A->Consume(Reacted * RateA, Product);
	B->Consume(Reacted * RateB, Product);

	if (Product && Magnitude >= MinMagnitude)
	{
		SpawnProduct(Product, Contact, Direction, Magnitude, SourceActor);
	}
}

void UARPGElementalReactionSubsystem::SpawnProduct(UARPGMagicElement* Product,
	const FVector& Contact, const FVector& Direction, float Magnitude, AActor* SourceActor)
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
	}

	Spawned->FinishSpawning(SpawnTransform);

	UE_LOG(LogARPGWorld, Verbose, TEXT("Reaction produced '%s' at %s (magnitude %.1f)"),
		*Product->ElementTag.ToString(), *Contact.ToCompactString(), Magnitude);
}
