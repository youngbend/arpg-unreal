// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGHitboxComponent.h"
#include "ARPGHitStopComponent.h"
#include "ARPGCombat.h"
#include "ARPGCombatLibrary.h"
#include "ARPGDamageGameplayEffect.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGGameplayEffectContext.h"
#include "ARPGGameplayTags.h"
#include "ARPGHurtboxComponent.h"
#include "ARPGOffenseSet.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "DrawDebugHelpers.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"

UARPGHitboxComponent::UARPGHitboxComponent()
{
	// Only while armed. A hitbox spends almost all of its life disarmed, and the
	// tick's first act was to return on exactly that check.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;

	DamageEffectClass = UARPGDamageGameplayEffect::StaticClass();

	// Pawns by default. Destructibles and props add ObjectTypeQuery for
	// PhysicsBody on the specific hitbox rather than paying for it everywhere.
	TraceObjectTypes.Add(UEngineTypes::ConvertToObjectType(ECC_Pawn));
}

UARPGHitboxComponent* UARPGHitboxComponent::FindOnActor(const AActor* Actor,
	EARPGHitboxSource Source, bool bAllowFallback)
{
	if (!Actor)
	{
		return nullptr;
	}

	TArray<UARPGHitboxComponent*> Hitboxes;
	Actor->GetComponents<UARPGHitboxComponent>(Hitboxes);

	for (UARPGHitboxComponent* Hitbox : Hitboxes)
	{
		if (Hitbox->HitboxSource == Source)
		{
			return Hitbox;
		}
	}

	return (bAllowFallback && Hitboxes.Num() > 0) ? Hitboxes[0] : nullptr;
}

void UARPGHitboxComponent::ActivateHitbox()
{
	bArmed = true;
	TickAccumulator = 0.f;
	HitTargets.Reset();
	DeferredSelfTargets.Reset();
	SetComponentTickEnabled(true);

	PreviousLocation = GetComponentLocation();

	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	// Anything of OURS already inside the hitbox at the moment of arming is
	// deferred rather than hit. Resolved here, on the arming edge, because
	// "was already overlapping when this started" is only answerable now.
	AActor* Source = ResolveSourceActor();
	if (!Source)
	{
		return;
	}

	TArray<FOverlapResult> Overlaps;
	FCollisionObjectQueryParams ObjectParams;
	for (const TEnumAsByte<EObjectTypeQuery>& ObjectType : TraceObjectTypes)
	{
		ObjectParams.AddObjectTypesToQuery(UEngineTypes::ConvertToCollisionChannel(ObjectType));
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(ARPGHitboxArm), /*bTraceComplex=*/false);

	if (GetWorld()->OverlapMultiByObjectType(Overlaps, PreviousLocation, FQuat::Identity,
			ObjectParams, FCollisionShape::MakeSphere(TraceRadius), QueryParams))
	{
		for (const FOverlapResult& Overlap : Overlaps)
		{
			AActor* OverlapActor = Overlap.GetActor();
			if (OverlapActor && OverlapActor == Source)
			{
				DeferredSelfTargets.Add(OverlapActor);
			}
		}
	}
}

void UARPGHitboxComponent::DeactivateHitbox()
{
	bArmed = false;
	SetComponentTickEnabled(false);
}

void UARPGHitboxComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bArmed)
	{
		return;
	}

	// Damage is server-only. The component still ticks on clients so cosmetic
	// hooks can run, but it must never resolve a hit there.
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		PreviousLocation = GetComponentLocation();
		return;
	}

	// Re-hit ticking for channelled attacks: clearing the per-activation set is
	// what allows a still-contacting target to be damaged again.
	if (TickInterval > 0.f)
	{
		TickAccumulator += DeltaTime;
		if (TickAccumulator >= TickInterval)
		{
			TickAccumulator -= TickInterval;
			HitTargets.Reset();
		}
	}

	PerformSweep();
	PreviousLocation = GetComponentLocation();
}

void UARPGHitboxComponent::PerformSweep()
{
	const FVector Start = PreviousLocation;
	const FVector End = GetComponentLocation();

	FCollisionObjectQueryParams ObjectParams;
	for (const TEnumAsByte<EObjectTypeQuery>& ObjectType : TraceObjectTypes)
	{
		ObjectParams.AddObjectTypesToQuery(UEngineTypes::ConvertToCollisionChannel(ObjectType));
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(ARPGHitboxSweep), /*bTraceComplex=*/false);
	QueryParams.AddIgnoredActor(GetOwner());
	QueryParams.bReturnPhysicalMaterial = false;

	TArray<FHitResult> Hits;

	// Even when the component has not moved this frame (Start == End) the sweep
	// still reports overlaps at that point, so a stationary hazard keeps working.
	GetWorld()->SweepMultiByObjectType(Hits, Start, End, FQuat::Identity, ObjectParams,
		FCollisionShape::MakeSphere(TraceRadius), QueryParams);

#if ENABLE_DRAW_DEBUG
	if (bDrawDebugTrace)
	{
		const FColor SweepColour = Hits.Num() > 0 ? FColor::Green : FColor::Red;
		DrawDebugSphere(GetWorld(), Start, TraceRadius, 12, SweepColour, false, 1.f);
		if (!Start.Equals(End))
		{
			DrawDebugSphere(GetWorld(), End, TraceRadius, 12, SweepColour, false, 1.f);
			DrawDebugLine(GetWorld(), Start, End, SweepColour, false, 1.f);
		}
		for (const FHitResult& Hit : Hits)
		{
			DrawDebugPoint(GetWorld(), Hit.ImpactPoint, 12.f, FColor::Yellow, false, 1.f);
		}
	}
#endif

	AActor* Source = ResolveSourceActor();

	for (const FHitResult& Hit : Hits)
	{
		AActor* HitActor = Hit.GetActor();
		if (!HitActor)
		{
			continue;
		}

		if (HitTargets.Contains(HitActor))
		{
			continue; // already hit this activation
		}

		// Registry lookup rather than a component walk -- this runs for every
		// sweep result on every armed frame. Ordered after the cheap set test
		// above so a re-hit of the same target costs nothing at all.
		UARPGHurtboxComponent* Hurtbox = UARPGHurtboxComponent::FindFor(HitActor);
		if (!Hurtbox)
		{
			continue; // not a damageable thing
		}

		// A deferred self-target stays suppressed only while it keeps
		// overlapping. Since it IS in this sweep's results it is still in
		// contact, so it stays suppressed and we skip it.
		if (DeferredSelfTargets.Contains(HitActor))
		{
			continue;
		}

		if (!bIgnoreFactionFilter && !UARPGCombatLibrary::CanDamage(Source, HitActor))
		{
			continue;
		}

		if (!Hurtbox->TryConsumeHit())
		{
			continue; // i-frames
		}

		HitTargets.Add(HitActor);
		DeliverHit(Hurtbox, Hit);

		if (bOneShot)
		{
			DeactivateHitbox();
			return;
		}
	}

	// Anything deferred that is no longer in contact is released, so a caster
	// who walks back into their own lingering hazard can be hit by it.
	for (auto It = DeferredSelfTargets.CreateIterator(); It; ++It)
	{
		const bool bStillOverlapping = Hits.ContainsByPredicate(
			[&It](const FHitResult& H) { return H.GetActor() == It->Get(); });

		if (!It->IsValid() || !bStillOverlapping)
		{
			It.RemoveCurrent();
		}
	}
}

void UARPGHitboxComponent::DeliverHit(UARPGHurtboxComponent* Hurtbox, const FHitResult& Hit)
{
	UAbilitySystemComponent* SourceASC = ResolveSourceASC();
	UAbilitySystemComponent* TargetASC = Hurtbox->GetAbilitySystemComponent();

	if (!SourceASC || !TargetASC || !DamageEffectClass)
	{
		return;
	}

	// Rolled HERE, on the server, and carried on the context. Rolling it inside
	// the execution would be equally authoritative but would let the client's hit
	// reaction disagree with the number it is reacting to. The wielder's own crit
	// chance (archetype, weapon affixes, buffs) plus whatever this attack adds.
	//
	// Hoisted out of the context block so the elemental rider can be handed the
	// SAME roll: one contact crits or it does not, and a swing that crit with its
	// steel but not with its fire would be indefensible on screen.
	const float TotalCritChance = FMath::Clamp(
		SourceASC->GetNumericAttribute(UARPGOffenseSet::GetCritChanceAttribute()) + CriticalChance,
		0.f, 1.f);
	const bool bCritical = TotalCritChance > 0.f && FMath::FRand() < TotalCritChance;

	FGameplayEffectContextHandle ContextHandle = SourceASC->MakeEffectContext();
	ContextHandle.AddSourceObject(this);
	ContextHandle.AddHitResult(Hit);

	if (FARPGGameplayEffectContext* Context =
			FARPGGameplayEffectContext::ExtractFrom(ContextHandle))
	{
		Context->SourceHitbox = this;
		Context->DamageType = DamageType;
		Context->ContactPoint = Hit.ImpactPoint;
		Context->bHasContactPoint = true;
		Context->KnockbackDirection = (Hit.ImpactPoint - PreviousLocation).GetSafeNormal();
		Context->KnockbackForce = KnockbackForce;
		Context->PoiseDamage = PoiseDamage;
		Context->HitStopDuration = HitStopDuration;
		Context->Penetration = PenetrationOverride;
		Context->bUnblockable = bUnblockable;
		Context->MagicElementTag = MagicElementTag;
		Context->bIsCritical = bCritical;
	}

	const FGameplayEffectSpecHandle SpecHandle =
		SourceASC->MakeOutgoingSpec(DamageEffectClass, 1.f, ContextHandle);

	if (!SpecHandle.IsValid())
	{
		return;
	}

	SpecHandle.Data->SetSetByCallerMagnitude(TAG_Data_Damage, BaseDamage);

	SourceASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data, TargetASC);

	// Fired on the SERVER once the hit has actually been delivered, and
	// multicast from there -- so every machine freezes on the same hit rather
	// than each predicting its own. Purely presentational: it holds the pose,
	// not the character, and nothing downstream reads it.
	UARPGHitStopComponent::ApplyToPair(
		SourceASC->GetAvatarActor(), TargetASC->GetAvatarActor(), HitStopDuration);

	ApplyOnHitEffects(SourceASC, TargetASC, ContextHandle, OnHitEffects, OnHitEffectDuration);

	// The coating goes through the pipeline as its OWN hit, with its own damage
	// type -- see FARPGElementalRider. Last, so the physical blow and anything it
	// inflicted resolve first, exactly as the status effects do.
	if (!ElementalRider.IsEmpty())
	{
		DeliverElementalRider(SourceASC, TargetASC, Hit, bCritical);
	}

	// The PHYSICAL amount only. These feed progression and hit feedback, and an
	// imbued swing is not a bigger weapon hit than an uncoated one -- folding the
	// element in here would pay weapon proficiency for the spell.
	Hurtbox->OnHitReceived.Broadcast(ContextHandle, BaseDamage);
	OnHitLanded.Broadcast(Hit.GetActor(), Hit, BaseDamage);
}

void UARPGHitboxComponent::DeliverElementalRider(UAbilitySystemComponent* SourceASC,
	UAbilitySystemComponent* TargetASC, const FHitResult& Hit, bool bCritical)
{
	if (!DamageEffectClass)
	{
		return;
	}

	// A context of its own rather than a second spec off the physical one: the
	// damage type is the thing that has to differ, and it is carried on the
	// context, not the spec.
	FGameplayEffectContextHandle ContextHandle = SourceASC->MakeEffectContext();
	ContextHandle.AddSourceObject(this);
	ContextHandle.AddHitResult(Hit);

	if (FARPGGameplayEffectContext* Context =
			FARPGGameplayEffectContext::ExtractFrom(ContextHandle))
	{
		Context->SourceHitbox = this;
		Context->DamageType = ElementalRider.DamageType;
		Context->ContactPoint = Hit.ImpactPoint;
		Context->bHasContactPoint = true;
		Context->PoiseDamage = ElementalRider.PoiseDamage;
		Context->MagicElementTag = ElementalRider.MagicElementTag;
		Context->bIsCritical = bCritical;

		// Blockability follows the SWING. A coating cannot make an unblockable
		// attack blockable, or the reverse -- the player is reading the wind-up,
		// not the element.
		Context->bUnblockable = bUnblockable;

		// Left at the default so the ELEMENT's damage type decides how much
		// resistance is ignored. PenetrationOverride is the weapon's answer for
		// steel and has nothing to say about fire.
		Context->Penetration = -1.f;

		// KnockbackForce and HitStopDuration stay at zero, and no second
		// ApplyToPair call follows: the physical hit already shoved and froze.
	}

	const FGameplayEffectSpecHandle SpecHandle =
		SourceASC->MakeOutgoingSpec(DamageEffectClass, 1.f, ContextHandle);

	if (!SpecHandle.IsValid())
	{
		return;
	}

	SpecHandle.Data->SetSetByCallerMagnitude(TAG_Data_Damage, ElementalRider.BaseDamage);
	SourceASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data, TargetASC);

	ApplyOnHitEffects(SourceASC, TargetASC, ContextHandle,
		ElementalRider.OnHitEffects, ElementalRider.OnHitEffectDuration);
}

void UARPGHitboxComponent::ApplyOnHitEffects(UAbilitySystemComponent* SourceASC,
	UAbilitySystemComponent* TargetASC, const FGameplayEffectContextHandle& ContextHandle,
	const TArray<TSubclassOf<UGameplayEffect>>& Effects, float DurationOverride)
{
	// Applied AFTER the damage, and through the same context, so a status can be
	// read alongside the hit that carried it -- and so a target killed by the
	// damage does not also catch fire. Each gets its own spec rather than being
	// folded into the damage effect: they have their own durations, stacking and
	// application gating, none of which the damage pipeline knows about.
	for (const TSubclassOf<UGameplayEffect>& EffectClass : Effects)
	{
		if (!EffectClass)
		{
			continue;
		}

		const FGameplayEffectSpecHandle StatusSpec =
			SourceASC->MakeOutgoingSpec(EffectClass, 1.f, ContextHandle);
		if (!StatusSpec.IsValid())
		{
			continue;
		}

		if (DurationOverride > 0.f)
		{
			// SetDuration rather than a SetByCaller magnitude: the status effects
			// carry fixed authored durations, and overriding on the spec means an
			// element can retime one without every status having to be rebuilt
			// around a caller-supplied duration it usually does not want.
			// bLockDuration stops the effect recomputing it back on application.
			StatusSpec.Data->SetDuration(DurationOverride, /*bLockDuration=*/true);
		}

		SourceASC->ApplyGameplayEffectSpecToTarget(*StatusSpec.Data, TargetASC);
	}
}

AActor* UARPGHitboxComponent::ResolveSourceActor() const
{
	if (AActor* Override = SourceActorOverride.Get())
	{
		return Override;
	}
	return GetOwner();
}

UAbilitySystemComponent* UARPGHitboxComponent::ResolveSourceASC() const
{
	return UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(ResolveSourceActor());
}
