// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGElementalVolumeComponent.h"
#include "ARPGElementalReactive.h"
#include "ARPGGameplayTags.h"
#include "ARPGHitboxComponent.h"
#include "ARPGHurtboxComponent.h"
#include "ARPGMagic.h"
#include "ARPGMagicElement.h"
#include "AbilitySystemComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/OverlapResult.h"
#include "GameFramework/Actor.h"
#include "GameplayEffect.h"

FARPGOnVolumesMet UARPGElementalVolumeComponent::OnVolumesMet;

UARPGElementalVolumeComponent::UARPGElementalVolumeComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// Neither of the two things this ticks for is frame-critical: an ambient
	// application is already interval-gated, and immersion polling is asking
	// whether a projectile has finished falling into a reservoir. The engine's
	// own interval skips the dispatch entirely rather than entering the function
	// and returning, which is what a hand-rolled accumulator does.
	PrimaryComponentTick.TickInterval = 0.1f;

	SetIsReplicatedByDefault(false); // resolved server-side; effects replicate
}

void UARPGElementalVolumeComponent::BeginPlay()
{
	Super::BeginPlay();

	if (!OverlapSource && GetOwner())
	{
		OverlapSource = Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent());
	}

	if (!OverlapSource)
	{
		UE_LOG(LogARPGMagic, Warning,
			TEXT("Elemental volume on %s has no collider to watch, so it can never meet anything."),
			*GetNameSafe(GetOwner()));
		return;
	}

	OverlapSource->SetGenerateOverlapEvents(true);
	OverlapSource->OnComponentBeginOverlap.AddDynamic(
		this, &UARPGElementalVolumeComponent::HandleOverlapBegin);

	// A STATIC collider never has its overlap list updated by the engine, so
	// begin-overlap never fires for it and nothing arriving is ever noticed.
	// Conduction is unaffected -- GetOverlappingVolumes queries live rather than
	// reading the cache -- but a static reservoir will not react to a spell
	// landing in it, which is the thing a reservoir mostly exists to do.
	if (OverlapSource->Mobility == EComponentMobility::Static)
	{
		UE_LOG(LogARPGMagic, Warning,
			TEXT("Elemental volume on %s watches a STATIC collider, so it will never notice "
			     "anything entering it. Set its mobility to Movable."),
			*GetNameSafe(GetOwner()));
	}

	if (!bCapturedBaseScale && GetOwner())
	{
		BaseScale = GetOwner()->GetActorScale3D();
		bCapturedBaseScale = true;
	}
}

void UARPGElementalVolumeComponent::SetEnergy(float NewEnergy)
{
	Energy = FMath::Max(0.f, NewEnergy);

	// Until the first reaction this also RESETS the baseline. That matters for
	// ordering: an effect actor begins play -- and would otherwise fix its
	// baseline at zero -- before the discharge hands it the energy it takes from
	// its context.
	if (!bSpentAny)
	{
		InitialEnergy = Energy;
	}
}

bool UARPGElementalVolumeComponent::ContainsPoint(FVector WorldPoint, float BelowReach) const
{
	if (!OverlapSource)
	{
		return false;
	}

	const FBoxSphereBounds ShapeBounds = OverlapSource->Bounds;
	const FVector Origin = ShapeBounds.Origin;
	const FVector Extent = ShapeBounds.BoxExtent;

	// Horizontal containment is the broadphase question and is answered by the
	// collider directly.
	if (FMath::Abs(WorldPoint.X - Origin.X) > Extent.X ||
		FMath::Abs(WorldPoint.Y - Origin.Y) > Extent.Y)
	{
		return false;
	}

	const float Bottom = Origin.Z - Extent.Z;
	const float Surface = GetSurfaceHeightAt(WorldPoint);

	// The vertical test is where broadphase and truth diverge: for a reservoir
	// the point must be at or below the WATERLINE, not merely inside the tall
	// box that reaches well above it.
	return (WorldPoint.Z - BelowReach) <= Surface && WorldPoint.Z >= Bottom;
}

FVector UARPGElementalVolumeComponent::GetVolumeLocation() const
{
	return OverlapSource ? OverlapSource->Bounds.Origin : GetComponentLocation();
}

FVector UARPGElementalVolumeComponent::GetVolumeExtent() const
{
	return OverlapSource ? OverlapSource->Bounds.BoxExtent : FVector::ZeroVector;
}

float UARPGElementalVolumeComponent::GetSurfaceHeightAt(FVector WorldPoint) const
{
	if (!OverlapSource)
	{
		return 0.f;
	}

	const FBoxSphereBounds ShapeBounds = OverlapSource->Bounds;
	const float Top = ShapeBounds.Origin.Z + ShapeBounds.BoxExtent.Z;

	// An AUTHORED waterline, which is all a box can carry. Anything with a real
	// surface to report -- a water body with a spline that runs downhill --
	// overrides this rather than pretending one number covers its length.
	return bReservoir ? GetVolumeLocation().Z + SurfaceHeightOffset : Top;
}

TArray<UARPGElementalVolumeComponent*> UARPGElementalVolumeComponent::GetOverlappingVolumes() const
{
	TArray<UARPGElementalVolumeComponent*> Volumes;

	const UWorld* World = GetWorld();
	if (!OverlapSource || !World)
	{
		return Volumes;
	}

	// A LIVE QUERY, not the component's cached overlap list.
	//
	// That cache is maintained as a side effect of MOVEMENT, so it is empty for
	// anything standing still -- which is every reservoir, every puddle, and any
	// static collider. A conduction flood asks "what is touching this right now",
	// once, in a frame where nothing has necessarily moved; answering it from a
	// movement cache means a chain of stationary puddles conducts to nothing.
	// Conduction is rare enough that the query costs nothing worth saving.
	TArray<FOverlapResult> Overlaps;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ARPGElementalVolume), /*bTraceComplex=*/false);
	Params.AddIgnoredActor(GetOwner());

	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
	ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);
	ObjectParams.AddObjectTypesToQuery(ECC_Pawn);
	ObjectParams.AddObjectTypesToQuery(ECC_PhysicsBody);

	World->OverlapMultiByObjectType(Overlaps, GetVolumeLocation(),
		OverlapSource->GetComponentQuat(), ObjectParams,
		OverlapSource->GetCollisionShape(), Params);

	for (const FOverlapResult& Result : Overlaps)
	{
		AActor* OtherActor = Result.GetActor();
		if (!OtherActor || OtherActor == GetOwner())
		{
			continue;
		}

		if (UARPGElementalVolumeComponent* Volume =
				OtherActor->FindComponentByClass<UARPGElementalVolumeComponent>())
		{
			Volumes.AddUnique(Volume);
		}
	}

	return Volumes;
}

void UARPGElementalVolumeComponent::HandleOverlapBegin(UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
	bool bFromSweep, const FHitResult& SweepResult)
{
	if (!bMonitoring || !OtherActor || OtherActor == GetOwner())
	{
		return;
	}

	// Announced, not resolved. What two elements do to each other is the
	// reaction subsystem's business -- see OnVolumesMet.
	if (UARPGElementalVolumeComponent* Other =
			OtherActor->FindComponentByClass<UARPGElementalVolumeComponent>())
	{
		OnVolumesMet.Broadcast(this, Other);
	}
}

void UARPGElementalVolumeComponent::Consume(float Amount, UARPGMagicElement* Product)
{
	bSpentAny = true;

	if (!bReservoir)
	{
		Energy = FMath::Max(0.f, Energy - Amount);
	}

	const float Remaining = InitialEnergy > 0.f
		? Energy / InitialEnergy
		: (bReservoir ? 1.f : 0.f);

	if (!bReservoir && Energy <= 0.f)
	{
		// A spent projectile must not react again while its effect plays out its
		// impact and fades.
		bMonitoring = false;
		if (OverlapSource)
		{
			OverlapSource->SetGenerateOverlapEvents(false);
		}
	}

	OnReacted.Broadcast(Amount, Remaining, Product);
	NotifyOwner(Amount, Remaining, Product);
}

void UARPGElementalVolumeComponent::Amplify(float Amount, UARPGMagicElement* Product)
{
	if (Amount <= 0.f)
	{
		return;
	}

	bSpentAny = true;
	Energy += Amount;

	const float Remaining = InitialEnergy > 0.f ? Energy / InitialEnergy : 1.f;

	// Negative Consumed reads as a gain, and Remaining > 1 tells the effect to
	// grow rather than shrink -- see IARPGElementalReactive.
	OnReacted.Broadcast(-Amount, Remaining, Product);
	NotifyOwner(-Amount, Remaining, Product);
}

void UARPGElementalVolumeComponent::NotifyOwner(float Consumed, float Remaining,
	UARPGMagicElement* Product)
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	if (Owner->Implements<UARPGElementalReactive>())
	{
		IARPGElementalReactive::Execute_OnElementalReaction(Owner, Consumed, Remaining, Product);
		return;
	}

	ApplyDefaultReaction(Remaining);
}

void UARPGElementalVolumeComponent::ApplyDefaultReaction(float Remaining)
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	if (Remaining <= 0.01f)
	{
		Owner->Destroy();
		return;
	}

	TArray<UARPGHitboxComponent*> Hitboxes;
	Owner->GetComponents<UARPGHitboxComponent>(Hitboxes);
	for (UARPGHitboxComponent* Hitbox : Hitboxes)
	{
		Hitbox->BaseDamage *= Remaining;
		Hitbox->PoiseDamage *= Remaining;
	}

	// Cube root, so the thing loses that fraction of its VOLUME rather than its
	// width -- and grows correctly when amplified.
	//
	// Applied to the CAPTURED base scale rather than the current one. Compounding
	// off the current scale (as the Godot version did) shrinks a twice-reacted
	// volume faster than its energy actually fell: two reactions leaving 0.5 and
	// then 0.25 of the original would land at 0.5x linear instead of 0.63x.
	const float Linear = FMath::Pow(Remaining, 1.f / 3.f);
	Owner->SetActorScale3D(BaseScale * Linear);
}

void UARPGElementalVolumeComponent::ApplyAmbient()
{
	if (!Element || !OverlapSource)
	{
		return;
	}

	const float DamagePerTick = Element->AmbientDamagePerSecond * AmbientInterval;
	if (DamagePerTick <= 0.f && !Element->OnHitEffect)
	{
		return; // nothing to apply -- water that neither hurts nor wets
	}

	TArray<UPrimitiveComponent*> Overlapping;
	OverlapSource->GetOverlappingComponents(Overlapping);

	TSet<AActor*> Applied;
	for (const UPrimitiveComponent* Other : Overlapping)
	{
		AActor* OtherActor = Other ? Other->GetOwner() : nullptr;
		if (!OtherActor || OtherActor == GetOwner() || Applied.Contains(OtherActor))
		{
			continue;
		}

		// Registry lookup: this runs for every overlapping component of every
		// ambient volume, and most of them are not damageable at all.
		UARPGHurtboxComponent* Hurtbox = UARPGHurtboxComponent::FindFor(OtherActor);
		if (!Hurtbox)
		{
			continue;
		}

		// Their FEET, not their middle: standing waist-deep is being in the
		// water, and standing on the bank inside the collider is not.
		const FVector Location = OtherActor->GetActorLocation();
		float Reach = 0.f;
		if (const UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(OtherActor->GetRootComponent()))
		{
			Reach = FMath::Max(0.f, Location.Z - (Body->Bounds.Origin.Z - Body->Bounds.BoxExtent.Z));
		}

		if (!ContainsPoint(Location, Reach))
		{
			continue;
		}

		Applied.Add(OtherActor);

		UAbilitySystemComponent* TargetASC = Hurtbox->GetAbilitySystemComponent();
		if (!TargetASC || !Element->OnHitEffect)
		{
			continue;
		}

		FGameplayEffectContextHandle ContextHandle = TargetASC->MakeEffectContext();
		ContextHandle.AddSourceObject(this);

		const FGameplayEffectSpecHandle Spec =
			TargetASC->MakeOutgoingSpec(Element->OnHitEffect, 1.f, ContextHandle);
		if (Spec.IsValid())
		{
			TargetASC->ApplyGameplayEffectSpecToSelf(*Spec.Data);
		}
	}
}

void UARPGElementalVolumeComponent::PollImmersion()
{
	// Nothing to poll unless something can arrive at a surface below where it
	// entered, which is only true of a reservoir.
	if (!bReservoir || !bMonitoring)
	{
		return;
	}

	// GetOverlappingVolumes runs a four-channel physics overlap against this
	// body's whole shape. Its own comment justifies that cost on the grounds
	// that conduction is rare -- but this called it every single frame for every
	// reservoir in the level, which is the opposite of rare. The interval on the
	// component tick is what makes the comment true again.
	for (UARPGElementalVolumeComponent* Other : GetOverlappingVolumes())
	{
		if (!Other || Other->bReservoir || Other->GetEnergy() <= 0.f)
		{
			continue;
		}

		// Only once it has actually reached the water. The overlap fired while
		// the projectile was still metres in the air.
		if (ContainsPoint(Other->GetVolumeLocation()))
		{
			OnVolumesMet.Broadcast(this, Other);
		}
	}
}

void UARPGElementalVolumeComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	PollImmersion();

	if (bAmbientSource)
	{
		AmbientAccumulator += DeltaTime;
		if (AmbientAccumulator >= AmbientInterval)
		{
			AmbientAccumulator = 0.f;
			ApplyAmbient();
		}
	}
}
