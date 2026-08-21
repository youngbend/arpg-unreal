// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGPlaceholderEffect.h"
#include "ARPGElementPalette.h"
#include "ARPGHitboxComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "GameFramework/ProjectileMovementComponent.h"

namespace
{
	/**
	 * How much of a palette's emission strength a placeholder light takes.
	 *
	 * One shared factor across every stand-in, so the whole family reads as one
	 * brightness rather than the projectile looking like a different effect from
	 * the volume that produced it.
	 */
	constexpr float PlaceholderEmissionFactor = 0.4f;

	/** Segments on a drawn sphere. Enough to read as round, cheap enough to spam. */
	constexpr int32 DebugSegments = 16;

	FLinearColor ResolveVolumeColour(const UARPGElementPalette* Palette)
	{
		if (!Palette)
		{
			return FLinearColor::White;
		}

		// GLOW, not Core. Core is deliberately near-white -- a fine accent on a
		// small orb, but across a volume this size it bleaches the whole thing
		// toward white instead of reading as the element's actual colour. Glow
		// is what "the colour you would name it" means.
		return Palette->Glow;
	}
}

// ---------------------------------------------------------------------------
// Stationary volume
// ---------------------------------------------------------------------------

AARPGPlaceholderDischarge::AARPGPlaceholderDischarge()
{
	PrimaryActorTick.bCanEverTick = true;

	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetCastShadows(false);
	Light->SetIntensity(0.f); // until a palette says otherwise
}

FARPGPlaceholderShape AARPGPlaceholderDischarge::GetShapeFor(EARPGDischargeType Type)
{
	FARPGPlaceholderShape Result;

	switch (Type)
	{
	case EARPGDischargeType::Cloak:
		// Wraps the caster, so a sphere centred on them, and HELD -- the cloak
		// component owns when it ends.
		Result.MinRadius = 100.f;
		Result.MaxRadius = 220.f;
		Result.Hold = 0.f;
		Result.bHeld = true;
		break;

	case EARPGDischargeType::Burst:
		// A directional blast. Swept, because a sphere centred on the caster
		// would imply it hits behind them, which is the opposite of what a
		// burst does.
		Result.bSwept = true;
		Result.MinRadius = 60.f;
		Result.MaxRadius = 140.f;
		Result.MinLength = 250.f;
		Result.MaxLength = 550.f;
		Result.Hold = 0.2f;
		break;

	case EARPGDischargeType::Emanate:
		// A wide ring outward, so a large centred sphere.
		Result.MinRadius = 160.f;
		Result.MaxRadius = 500.f;
		Result.Hold = 0.25f;
		break;

	case EARPGDischargeType::Collision:
		// No caster to aim it -- the reaction subsystem releases it wherever two
		// volumes touched -- so centred like Emanate, but smaller and quicker: a
		// reaction is a single release, not a sustained ring.
		Result.MinRadius = 80.f;
		Result.MaxRadius = 300.f;
		Result.Hold = 0.2f;

		// AND IT DOES NOT REACT. A product is born AT the contact point, which
		// means inside whichever volume survived the trade -- and two different
		// elements neutralise even with no recipe between them. So a steam cloud
		// would immediately eat the fireball's surplus, which is the exact
		// behaviour SurplusSurvivesAtReducedPower exists to protect.
		//
		// The result of a reaction re-entering the solver where it was born is a
		// feedback loop, not a feature. An authored effect can still opt in.
		Result.bReacts = false;
		break;

	default:
		break;
	}

	return Result;
}

void AARPGPlaceholderDischarge::InitializeFromContext(const FARPGDischargeContext& InContext)
{
	Super::InitializeFromContext(InContext);

	Shape = GetShapeFor(InContext.DischargeType);

	// NORMALISED against the power window rather than read raw, which is what
	// lets mastery push a placeholder past its nominal size -- exactly the kind
	// of thing worth being able to see.
	const float Power = InContext.GetNormalisedPower();

	Radius = FMath::Lerp(Shape.MinRadius, Shape.MaxRadius, Power);
	TravelDistance = Shape.bSwept ? FMath::Lerp(Shape.MinLength, Shape.MaxLength, Power) : 0.f;

	// THE SAME NUMBER drives the hitbox and the drawn volume. Anything else and
	// the placeholder stops being a truthful picture of what the spell hits,
	// which is the only reason it exists.
	Hitbox->TraceRadius = Radius;

	// And the same number again for what it leaves on the ground, so an element
	// that pools wets exactly the ground the spell covered. A burst sweeps because
	// its volume genuinely does travel forward; an emanation and a cloak are
	// centred, so they wet where they stood. Elements that pool nothing -- fire,
	// air -- deposit nothing from this, with no branch here.
	DepositRadius = Radius;
	bDepositSwept = Shape.bSwept;

	// And again for what it MEETS. Three things now come off this one number --
	// what it hits, what it leaves, and what it reacts with -- which is the rule
	// this class already lived by, extended rather than bent.
	ReactionRadius = Shape.bReacts ? Radius : 0.f;

	// Written before FinishSpawning, so the base class's BeginPlay picks them up
	// -- this is the window the deferred spawn exists for.
	if (Shape.bHeld)
	{
		// Armed for the whole life and no lifetime of its own; see bHeld.
		HitboxWindow = 0.f;
		Lifetime = 0.f;
	}
	else
	{
		HitboxWindow = ExpandTime + Shape.Hold;
		Lifetime = ExpandTime + Shape.Hold + FadeTime;
	}

	StartLocation = InContext.Origin;
}

void AARPGPlaceholderDischarge::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	Elapsed += DeltaTime;

	const float Growth = FMath::Clamp(Elapsed / ExpandTime, 0.f, 1.f);

	if (Shape.bSwept && TravelDistance > 0.f)
	{
		// Moving the ACTOR moves the hitbox, which is the root -- so the sweep
		// between frames traces the capsule this burst really is. Nothing here
		// approximates the shape; the shape is the path.
		SetActorLocation(StartLocation + GetActorForwardVector() * (TravelDistance * Growth));
	}

#if ENABLE_DRAW_DEBUG
	// Drawn every frame rather than persisted, so it follows a swept burst and
	// disappears with the actor rather than outliving it.
	const float Drawn = Shape.bSwept ? Radius : Radius * FMath::Max(Growth, 0.01f);
	DrawDebugSphere(GetWorld(), GetActorLocation(), Drawn, DebugSegments,
		VolumeColour.ToFColor(/*bSRGB=*/true), /*bPersistent=*/false, /*LifeTime=*/-1.f);
#endif
}

void AARPGPlaceholderDischarge::ApplyPalette_Implementation(const UARPGElementPalette* Palette)
{
	VolumeColour = ResolveVolumeColour(Palette);

	if (Light)
	{
		Light->SetLightColor(VolumeColour);
		Light->SetAttenuationRadius(FMath::Max(Radius * 2.f, 200.f));
		Light->SetIntensity(Palette
			? Palette->EmissionStrength * PlaceholderEmissionFactor * 1000.f
			: 0.f);
	}
}

// ---------------------------------------------------------------------------
// Projectile
// ---------------------------------------------------------------------------

AARPGPlaceholderProjectile::AARPGPlaceholderProjectile()
{
	PrimaryActorTick.bCanEverTick = true;

	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetCastShadows(false);
	Light->SetIntensity(0.f);

	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Movement"));
	Movement->bRotationFollowsVelocity = true;

	// THE VELOCITY BELOW IS A WORLD DIRECTION, and the component has to be told
	// so. UProjectileMovementComponent defaults bInitialVelocityInLocalSpace to
	// TRUE: on initialise it takes whatever is in Velocity and transforms it by
	// the actor's rotation. The discharge spawns this actor ALREADY FACING the
	// aim, so a world-space velocity was being turned by the aim a second time --
	// fire at 40 degrees and the bolt left at 80, which reads as no direction at
	// all rather than as a consistent offset.
	//
	// Invisible down +X, which is the one heading where the spawn rotation is the
	// identity and the double turn is a no-op. See
	// Placeholder.AProjectileFliesTheWayItWasAimed, which aims off-axis for
	// exactly that reason.
	Movement->bInitialVelocityInLocalSpace = false;

	// No arc. A placeholder that dropped would make range look shorter than the
	// real spell's, and range is most of what a projectile is for.
	Movement->ProjectileGravityScale = 0.f;

	// One shot: a projectile that kept hitting as it passed through would report
	// damage the real thing never deals.
	Movement->bShouldBounce = false;
}

void AARPGPlaceholderProjectile::InitializeFromContext(const FARPGDischargeContext& InContext)
{
	Super::InitializeFromContext(InContext);

	const float Power = InContext.GetNormalisedPower();

	Radius = FMath::Lerp(MinRadius, MaxRadius, Power);
	Hitbox->TraceRadius = Radius;

	// A thrown volume of liquid SPREADS where it lands rather than staying its own
	// width, which is the difference between a bolt leaving a puddle and a bolt
	// leaving a wet coin too small for the fluid system to keep. Not swept: what a
	// projectile wet is where it landed, not its line of flight.
	DepositRadius = Radius * DepositSpread;

	// The orb's own size, unspread: what it splashes is wider than what it is, but
	// what it MEETS is exactly what it is.
	ReactionRadius = Radius;

	// The hitbox stays armed for the whole flight and stops on the first
	// contact, which is what a projectile is: one hit, wherever it lands.
	Hitbox->bOneShot = true;
	HitboxWindow = 0.f;

	if (Movement)
	{
		const FVector Direction = InContext.Direction.GetSafeNormal();
		Movement->Velocity = Direction * Speed;
		Movement->InitialSpeed = Speed;
		Movement->MaxSpeed = Speed;
	}
}

void AARPGPlaceholderProjectile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

#if ENABLE_DRAW_DEBUG
	DrawDebugSphere(GetWorld(), GetActorLocation(), Radius, DebugSegments,
		VolumeColour.ToFColor(/*bSRGB=*/true), /*bPersistent=*/false, /*LifeTime=*/-1.f);
#endif
}

void AARPGPlaceholderProjectile::ApplyPalette_Implementation(const UARPGElementPalette* Palette)
{
	// CORE here, unlike the volume: on an orb this small a near-white centre
	// reads as hot rather than washing anything out, which is the whole reason
	// the palette carries a separate core colour.
	VolumeColour = Palette ? Palette->Core : FLinearColor::White;

	if (Light)
	{
		Light->SetLightColor(Palette ? Palette->Glow : FLinearColor::White);
		Light->SetAttenuationRadius(FMath::Max(Radius * 8.f, 400.f));
		Light->SetIntensity(Palette
			? Palette->EmissionStrength * PlaceholderEmissionFactor * 1000.f
			: 0.f);
	}
}

// ---------------------------------------------------------------------------
// Attached (hand / imbue)
// ---------------------------------------------------------------------------

AARPGPlaceholderAttachedEffect::AARPGPlaceholderAttachedEffect()
{
	PrimaryActorTick.bCanEverTick = true;

	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));

	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetCastShadows(false);
	Light->SetIntensity(0.f);
}

void AARPGPlaceholderAttachedEffect::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

#if ENABLE_DRAW_DEBUG
	DrawDebugSphere(GetWorld(), GetActorLocation(), Radius, 8,
		VolumeColour.ToFColor(/*bSRGB=*/true), /*bPersistent=*/false, /*LifeTime=*/-1.f);
#endif
}

void AARPGPlaceholderAttachedEffect::ApplyPalette_Implementation(const UARPGElementPalette* Palette)
{
	VolumeColour = Palette ? Palette->Core : FLinearColor::White;

	if (Light)
	{
		Light->SetLightColor(Palette ? Palette->Glow : FLinearColor::White);
		Light->SetAttenuationRadius(FMath::Max(Radius * 12.f, 150.f));
		Light->SetIntensity(Palette
			? Palette->EmissionStrength * PlaceholderEmissionFactor * 200.f
			: 0.f);
	}
}
