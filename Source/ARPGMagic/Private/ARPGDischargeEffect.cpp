// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGDischargeEffect.h"
#include "ARPGElementTintable.h"
#include "ARPGElementalVolumeComponent.h"
#include "ARPGHitboxComponent.h"
#include "ARPGMagic.h"
#include "ARPGMagicElement.h"
#include "Components/SphereComponent.h"
#include "GameplayEffect.h"
#include "TimerManager.h"

FARPGOnDischargeLanded AARPGDischargeEffect::OnDischargeLanded;

AARPGDischargeEffect::AARPGDischargeEffect()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;

	Hitbox = CreateDefaultSubobject<UARPGHitboxComponent>(TEXT("Hitbox"));
	SetRootComponent(Hitbox);

	// A spell hits whoever it touches, including the caster's own side where the
	// element says so -- the faction filter is the hitbox's job, and a discharge
	// keeps the default behaviour rather than opting out.
	Hitbox->bOneShot = false;

	Collider = CreateDefaultSubobject<USphereComponent>(TEXT("ReactionCollider"));
	Collider->SetupAttachment(Hitbox);

	// MOVABLE, or the engine never refreshes its overlap list and begin-overlap
	// never fires -- the volume component warns about exactly this, and a
	// projectile is the last thing that can afford to be static.
	Collider->SetMobility(EComponentMobility::Movable);

	// OVERLAP EVERYTHING, BLOCK NOTHING. This exists to notice other elements, not
	// to stop the spell flying: a collider that blocked would have the fireball
	// bouncing off the water it is supposed to react with.
	Collider->SetCollisionObjectType(ECC_WorldDynamic);
	Collider->SetCollisionResponseToAllChannels(ECR_Overlap);

	// Off until a context says otherwise. Most spells have no reaction radius, and
	// a live collider on every one of them is overlap traffic for nothing.
	Collider->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	Volume = CreateDefaultSubobject<UARPGElementalVolumeComponent>(TEXT("Volume"));
	Volume->SetupAttachment(Collider);
	Volume->OverlapSource = Collider;
}

void AARPGDischargeEffect::InitializeFromContext(const FARPGDischargeContext& InContext)
{
	Context = InContext;

	const UARPGMagicElement* Element = Context.PrimaryElement;
	if (!Element)
	{
		UE_LOG(LogARPGMagic, Warning,
			TEXT("Discharge effect '%s' spawned with no primary element; it will deal no damage."),
			*GetName());
		return;
	}

	// Precomputed on the context -- see FARPGDischargeContext. Recomputing here
	// is what let a spell silently deal uncharged damage in the Godot version.
	Hitbox->BaseDamage = Context.ComputedDamage;
	Hitbox->PoiseDamage = Context.ComputedPoiseDamage;
	Hitbox->DamageType = Element->DamageType;
	Hitbox->MagicElementTag = Element->ElementTag;

	if (Element->OnHitEffect)
	{
		Hitbox->OnHitEffects.AddUnique(Element->OnHitEffect);
		Hitbox->OnHitEffectDuration = Element->StatusDuration;
	}

	// Colour comes from the same place damage does, and for the same reason:
	// an effect asset should be art, not a place that re-reads the element.
	ARPGElementTint::Apply(this, Element);

	// The caster, not this actor: a fireball must not self-attribute its own
	// damage, or faction filtering treats it as its own faction and kill credit
	// goes to the projectile.
	Hitbox->SetSourceActor(Context.Caster);

	OnDischargeInitialized(Context);
}

void AARPGDischargeEffect::ConfigureReactionVolume()
{
	if (!Collider || !Volume)
	{
		return;
	}

	// Reactions are resolved server-side, and the effect replicates down, so a
	// client generating its own overlaps is traffic whose every conclusion the
	// reaction subsystem then discards for lack of authority.
	if (ReactionRadius <= 0.f || !Context.PrimaryElement || !HasAuthority())
	{
		return;
	}

	Collider->SetSphereRadius(ReactionRadius);

	// EVERYTHING THE VOLUME IS, BEFORE THE COLLIDER CAN BE SEEN. Switching
	// collision on is not a passive setting -- it makes the engine refresh the
	// component's overlaps there and then, and any it finds fire
	// OnComponentBeginOverlap synchronously, inside the SetCollisionEnabled call.
	//
	// SO ORDER IS THE WHOLE BUG. With the assignments below made AFTER that call,
	// as they were, a spell that spawned already overlapping something announced
	// itself carrying no element and no energy: the solver read an inert volume,
	// bailed at its first guard, and the pair was never looked at again -- because
	// BEGIN-overlap only fires on arrival, and by the next frame the two were
	// already overlapping. Nothing retries. An emanation cast over a puddle is
	// exactly that case, and it did nothing at all.
	//
	// A projectile hid this completely. Its volume travels, so it meets things
	// after BeginPlay has long finished and is fully armed by the time it does.
	// Only a spell born inside what it should react with was affected -- which is
	// every emanation, every cloak, and any burst cast at your own feet.
	Volume->Element = Context.PrimaryElement;

	// THE SAME NUMBER AS THE DAMAGE, which is the whole reason ComputedDamage is
	// precomputed on the context: a reaction then already accounts for charge,
	// mastery and buffs without re-deriving any of them, and a heavily charged
	// fireball genuinely out-trades a tapped one.
	Volume->SetEnergy(Context.ComputedDamage);

	// The caster, not this actor -- reaction damage attributes the same way hit
	// damage does, and self-attribution would give kill credit to the projectile.
	Volume->SourceActor = Context.Caster;

	// LAST, so the first overlap anyone hears about is of a volume that already
	// knows what it is.
	Collider->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
}

void AARPGDischargeEffect::BeginPlay()
{
	// BEFORE Super, which is what dispatches the volume component's own BeginPlay
	// -- and that is where it binds to the collider's overlap events. Arm it after
	// and the volume binds to a collider that is still switched off.
	ConfigureReactionVolume();

	Super::BeginPlay();

	// Damage is server-authoritative, so only the server arms anything; clients
	// have this actor purely for its art.
	if (HasAuthority())
	{
		Hitbox->ActivateHitbox();

		if (HitboxWindow > 0.f)
		{
			FTimerHandle Handle;
			GetWorldTimerManager().SetTimer(Handle, [this]()
			{
				if (IsValid(this))
				{
					Hitbox->DeactivateHitbox();
				}
			}, HitboxWindow, false);
		}
	}

	if (Lifetime > 0.f)
	{
		SetLifeSpan(Lifetime);
	}
}

void AARPGDischargeEffect::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// DESTROYED specifically, which is the spell FINISHING: its Blueprint killed
	// it on impact, or its lifespan ran out. Every other reason is the world going
	// away underneath it -- a level transition, PIE ending -- and leaving a puddle
	// behind on the way out is exactly what the reason enum exists to prevent.
	//
	// Broadcast before Super, which is where the components end and the actor
	// stops being able to answer where it is.
	if (EndPlayReason == EEndPlayReason::Destroyed && DepositRadius > 0.f)
	{
		OnDischargeLanded.Broadcast(this);
	}

	Super::EndPlay(EndPlayReason);
}
