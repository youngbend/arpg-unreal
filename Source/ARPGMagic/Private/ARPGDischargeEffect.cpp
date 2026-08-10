// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGDischargeEffect.h"
#include "ARPGElementTintable.h"
#include "ARPGHitboxComponent.h"
#include "ARPGMagic.h"
#include "ARPGMagicElement.h"
#include "GameplayEffect.h"
#include "TimerManager.h"

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

void AARPGDischargeEffect::BeginPlay()
{
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
