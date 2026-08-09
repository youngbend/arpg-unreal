// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGConductionSubsystem.h"
#include "ARPGDamageGameplayEffect.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGGameplayEffectContext.h"
#include "ARPGGameplayTags.h"
#include "ARPGHurtboxComponent.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicElement.h"
#include "ARPGWorld.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameplayEffect.h"

bool UARPGConductionSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

bool UARPGConductionSubsystem::Conducts(const FGameplayTag& ChargeTag,
	const FGameplayTag& MediumTag) const
{
	if (!CombinationTable || !ChargeTag.IsValid() || !MediumTag.IsValid())
	{
		return false;
	}

	FGameplayTagContainer Pair;
	Pair.AddTag(ChargeTag);
	Pair.AddTag(MediumTag);

	const UARPGMagicCombinationEntry* Entry =
		CombinationTable->ResolveEntry(Pair, EARPGCombinationScope::Collision);

	// Water carries lightning; stone does not. Which is which is authored, not
	// coded -- the row has to both exist and name this charge as what travels.
	return Entry && Entry->GetConductedElement() == ChargeTag;
}

int32 UARPGConductionSubsystem::Conduct(UARPGMagicElement* ChargeElement,
	UARPGElementalVolumeComponent* EntryMedium, float Energy, AActor* SourceActor)
{
	LastReachedCount = 0;
	LastDeliveredDamage = 0.f;

	if (!ChargeElement || !EntryMedium || Energy < MinEnergy)
	{
		return 0;
	}

	const FGameplayTag ChargeTag = ChargeElement->ElementTag;

	struct FStep
	{
		UARPGElementalVolumeComponent* Volume = nullptr;
		float Energy = 0.f;
		int32 Hops = 0;
	};

	// Breadth-first, so energy reaching a medium is always by the SHORTEST route
	// rather than whichever branch happened to be walked first. Depth-first would
	// make a puddle's damage depend on iteration order.
	TArray<FStep> Frontier;
	TArray<FStep> Next;
	TSet<UARPGElementalVolumeComponent*> Visited;

	Frontier.Add({ EntryMedium, Energy * EntryMedium->Conductivity, 0 });
	Visited.Add(EntryMedium);

	while (Frontier.Num() > 0)
	{
		Next.Reset();

		for (const FStep& Step : Frontier)
		{
			if (!Step.Volume || Step.Energy < MinEnergy)
			{
				continue;
			}

			++LastReachedCount;

			// Damage lands NOW, in the frame the charge arrives. That
			// instantaneity is the whole point of conduction -- only the visible
			// arcs would be paced out behind it.
			StrikeTargets(Step.Volume, ChargeElement, Step.Energy, SourceActor);

			if (Step.Hops >= MaxHops)
			{
				continue;
			}

			// Neighbours are simply the media this one overlaps. Nothing authors
			// the connection; touching IS the connection.
			for (UARPGElementalVolumeComponent* Neighbour : Step.Volume->GetOverlappingVolumes())
			{
				if (!Neighbour || Neighbour->Conductivity <= 0.f || Visited.Contains(Neighbour))
				{
					continue; // a projectile, or anything that does not carry
				}

				const UARPGMagicElement* MediumElement = Neighbour->Element;
				if (!MediumElement || !Conducts(ChargeTag, MediumElement->ElementTag))
				{
					continue;
				}

				Visited.Add(Neighbour);

				// Each hop costs the receiving medium's conductivity, and
				// crossing it costs distance -- so a charge fades along a chain
				// instead of arriving everywhere at full strength.
				const float Span = FVector::Dist(
					Neighbour->GetVolumeLocation(), Step.Volume->GetVolumeLocation());
				const float Arrived = Step.Energy * Neighbour->Conductivity
					* FMath::Max(0.f, 1.f - Span * DistanceLoss);

				if (Arrived >= MinEnergy)
				{
					Next.Add({ Neighbour, Arrived, Step.Hops + 1 });
				}
			}
		}

		Frontier = Next;
	}

	UE_LOG(LogARPGWorld, Verbose,
		TEXT("Conducted '%s' through %d medium(s), delivering %.1f damage."),
		*ChargeTag.ToString(), LastReachedCount, LastDeliveredDamage);

	return LastReachedCount;
}

void UARPGConductionSubsystem::StrikeTargets(UARPGElementalVolumeComponent* Medium,
	UARPGMagicElement* ChargeElement, float Energy, AActor* SourceActor)
{
	if (!Medium || !Medium->OverlapSource)
	{
		return;
	}

	TArray<UPrimitiveComponent*> Overlapping;
	Medium->OverlapSource->GetOverlappingComponents(Overlapping);

	// Snapshotted into a set first: delivering damage can kill something, which
	// destroys actors and mutates the overlap list mid-iteration.
	TSet<AActor*> Targets;
	for (const UPrimitiveComponent* Other : Overlapping)
	{
		if (AActor* OtherActor = Other ? Other->GetOwner() : nullptr)
		{
			Targets.Add(OtherActor);
		}
	}

	const float Damage = Energy * DamageScale;

	for (AActor* Target : Targets)
	{
		if (!IsValid(Target))
		{
			continue;
		}

		UARPGHurtboxComponent* Hurtbox = Target->FindComponentByClass<UARPGHurtboxComponent>();
		if (!Hurtbox)
		{
			continue;
		}

		// Their FEET, not their middle -- standing in the water is being in the
		// water, and standing on the bank inside the collider is not. This is the
		// broadphase-vs-truth split; skipping it is what shocked someone standing
		// dry on the shore.
		const FVector Location = Target->GetActorLocation();
		float Reach = 0.f;
		if (const UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(Target->GetRootComponent()))
		{
			Reach = FMath::Max(0.f, Location.Z - (Body->Bounds.Origin.Z - Body->Bounds.BoxExtent.Z));
		}

		if (!Medium->ContainsPoint(Location, Reach))
		{
			continue;
		}

		// Routed through the hurtbox so i-frames and dodging still apply -- the
		// same rule the plan sets for environmental damage generally.
		if (!Hurtbox->TryConsumeHit())
		{
			continue;
		}

		UAbilitySystemComponent* TargetASC = Hurtbox->GetAbilitySystemComponent();
		if (!TargetASC)
		{
			continue;
		}

		LastDeliveredDamage += Damage;

		// Attributed to whoever cast the spell that entered the medium, not to
		// the water: kill credit and faction filtering both depend on it, and a
		// puddle has no side.
		UAbilitySystemComponent* SourceASC = SourceActor
			? UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(SourceActor)
			: nullptr;
		UAbilitySystemComponent* Applier = SourceASC ? SourceASC : TargetASC;

		FGameplayEffectContextHandle ContextHandle = Applier->MakeEffectContext();
		ContextHandle.AddSourceObject(Medium);

		if (FARPGGameplayEffectContext* Context =
				static_cast<FARPGGameplayEffectContext*>(ContextHandle.Get()))
		{
			Context->DamageType = ChargeElement->DamageType;
			Context->MagicElementTag = ChargeElement->ElementTag;
			Context->ContactPoint = Location;
			Context->bHasContactPoint = true;

			// Conduction cannot be blocked or parried. There is no incoming blow
			// to read and no direction to face -- the charge is already in the
			// water you are standing in.
			Context->bUnblockable = true;
		}

		// The SAME execution every other hit runs through, so resistance,
		// armour and poise all behave identically to being hit by the spell
		// itself. A parallel damage path here would be a second place for the
		// mitigation order to drift.
		const FGameplayEffectSpecHandle DamageSpec =
			Applier->MakeOutgoingSpec(UARPGDamageGameplayEffect::StaticClass(), 1.f, ContextHandle);
		if (DamageSpec.IsValid())
		{
			DamageSpec.Data->SetSetByCallerMagnitude(TAG_Data_Damage, Damage);
			Applier->ApplyGameplayEffectSpecToTarget(*DamageSpec.Data, TargetASC);
		}

		if (!ChargeElement->OnHitEffect)
		{
			continue;
		}

		const FGameplayEffectSpecHandle StatusSpec =
			Applier->MakeOutgoingSpec(ChargeElement->OnHitEffect, 1.f, ContextHandle);
		if (StatusSpec.IsValid())
		{
			if (ChargeElement->StatusDuration > 0.f)
			{
				StatusSpec.Data->SetDuration(ChargeElement->StatusDuration, /*bLockDuration=*/true);
			}
			Applier->ApplyGameplayEffectSpecToTarget(*StatusSpec.Data, TargetASC);
		}
	}
}
