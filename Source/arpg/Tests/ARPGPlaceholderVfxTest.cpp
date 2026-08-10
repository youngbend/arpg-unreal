// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ARPGCombatDummy.h"
#include "ARPGElementPalette.h"
#include "ARPGElementTintable.h"
#include "ARPGGameplayTags.h"
#include "ARPGHandVisualComponent.h"
#include "ARPGHitboxComponent.h"
#include "ARPGMagicComponent.h"
#include "ARPGMagicCombinationTable.h"
#include "ARPGMagicElement.h"
#include "ARPGMagicLoadout.h"
#include "ARPGMagicSettings.h"
#include "ARPGPlaceholderEffect.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

/**
 * The stand-in visuals every unauthored element falls back to.
 *
 * These are "only" placeholders, which makes it tempting to leave untested. The
 * reason not to is that their entire value rests on ONE invariant -- that the
 * volume you see is the volume that hits -- and a placeholder that lies about
 * reach is worse than none at all, because it is lying at exactly the moment
 * someone is tuning against it.
 */
namespace ARPGPlaceholderTestUtils
{
	struct FTestWorld
	{
		UWorld* World = nullptr;

		FTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
		}

		~FTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};

	UARPGElementPalette* MakePalette(UObject* Outer, FLinearColor Glow)
	{
		UARPGElementPalette* Palette = NewObject<UARPGElementPalette>(Outer);
		Palette->Glow = Glow;
		Palette->Core = FLinearColor::White;
		Palette->EmissionStrength = 2.f;
		return Palette;
	}

	UARPGMagicElement* MakeElement(UObject* Outer, FGameplayTag Tag, bool bWithPalette)
	{
		UARPGMagicElement* Element = NewObject<UARPGMagicElement>(Outer);
		Element->ElementTag = Tag;
		Element->BaseDamage = 10.f;
		Element->ActivationCost = 0.f;
		Element->UsageRate = 1.f;
		if (bWithPalette)
		{
			Element->Palette = MakePalette(Element, FLinearColor(1.f, 0.2f, 0.f, 1.f));
		}
		return Element;
	}

	/** Spawns a placeholder the way the discharge ability does: deferred, then initialised. */
	template <typename TEffect>
	TEffect* SpawnPlaceholder(UWorld* World, const FARPGDischargeContext& Context)
	{
		const FTransform Transform(Context.Direction.Rotation(), Context.Origin);

		TEffect* Effect = World->SpawnActorDeferred<TEffect>(
			TEffect::StaticClass(), Transform, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);

		if (Effect)
		{
			Effect->InitializeFromContext(Context);
			Effect->FinishSpawning(Transform);
		}
		return Effect;
	}

	FARPGDischargeContext MakeContext(EARPGDischargeType Type, UARPGMagicElement* Element,
		float Power)
	{
		FARPGDischargeContext Context;
		Context.DischargeType = Type;
		Context.PrimaryElement = Element;
		Context.Origin = FVector::ZeroVector;
		Context.Direction = FVector::ForwardVector;
		Context.ComputedDamage = 25.f;
		Context.MinPower = 0.f;
		Context.MaxPower = 1.f;
		Context.PowerFraction = Power;
		return Context;
	}
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPlaceholderShapeTest,
	"ARPG.Magic.Placeholder.DrawnVolumeIsExactlyTheHitbox",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPlaceholderShapeTest::RunTest(const FString& Parameters)
{
	using namespace ARPGPlaceholderTestUtils;

	FTestWorld Fixture;
	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire, true);

	// THE INVARIANT. One number drives the volume and the hitbox; the moment
	// they can disagree the placeholder starts lying about reach, which is the
	// only thing it is for.
	for (const EARPGDischargeType Type : { EARPGDischargeType::Cloak, EARPGDischargeType::Burst,
		EARPGDischargeType::Emanate, EARPGDischargeType::Collision })
	{
		AARPGPlaceholderDischarge* Effect = SpawnPlaceholder<AARPGPlaceholderDischarge>(
			Fixture.World, MakeContext(Type, Fire, 1.f));

		TestNotNull(TEXT("The placeholder spawns"), Effect);
		if (!Effect)
		{
			return false;
		}

		TestEqual(TEXT("The drawn radius IS the trace radius"),
			Effect->GetVolumeRadius(), Effect->Hitbox->TraceRadius);
	}

	// Each type gets a shape that reads its reach honestly, and they genuinely
	// differ -- a cloak that measured like an emanation would misreport the
	// single most important thing about it.
	const FARPGPlaceholderShape Cloak = AARPGPlaceholderDischarge::GetShapeFor(EARPGDischargeType::Cloak);
	const FARPGPlaceholderShape Burst = AARPGPlaceholderDischarge::GetShapeFor(EARPGDischargeType::Burst);
	const FARPGPlaceholderShape Emanate = AARPGPlaceholderDischarge::GetShapeFor(EARPGDischargeType::Emanate);

	TestTrue(TEXT("An emanation reaches further than a cloak"),
		Emanate.MaxRadius > Cloak.MaxRadius);
	TestTrue(TEXT("A burst is thrown forward rather than sitting still"), Burst.bSwept);
	TestFalse(TEXT("An emanation is not"), Emanate.bSwept);

	// A cloak's lifetime belongs to the cloak component. Owning one here would
	// make it vanish mid-cloak, and disarming would stop it damaging what walks
	// into it -- both of which read as the cloak being broken.
	TestTrue(TEXT("A cloak is held by whoever cast it"), Cloak.bHeld);
	TestFalse(TEXT("A burst owns its own brief life"), Burst.bHeld);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPlaceholderPowerTest,
	"ARPG.Magic.Placeholder.PowerScalesTheVolume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPlaceholderPowerTest::RunTest(const FString& Parameters)
{
	using namespace ARPGPlaceholderTestUtils;

	FTestWorld Fixture;
	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire, true);

	AARPGPlaceholderDischarge* Weak = SpawnPlaceholder<AARPGPlaceholderDischarge>(
		Fixture.World, MakeContext(EARPGDischargeType::Emanate, Fire, 0.f));
	AARPGPlaceholderDischarge* Strong = SpawnPlaceholder<AARPGPlaceholderDischarge>(
		Fixture.World, MakeContext(EARPGDischargeType::Emanate, Fire, 1.f));

	if (!Weak || !Strong)
	{
		return false;
	}

	// A charged spell should visibly cover more ground, and the hitbox has to
	// agree -- that is what makes charging legible at all while tuning.
	TestTrue(TEXT("A fully charged emanation is bigger than a minimal one"),
		Strong->GetVolumeRadius() > Weak->GetVolumeRadius());
	TestEqual(TEXT("And the hitbox grew with it"),
		Strong->Hitbox->TraceRadius, Strong->GetVolumeRadius());

	// Normalised against the power WINDOW rather than read raw, which is what
	// lets mastery push a placeholder past its nominal size instead of pinning
	// every cast at the same reach.
	FARPGDischargeContext Wide = MakeContext(EARPGDischargeType::Emanate, Fire, 5.f);
	Wide.MinPower = 0.f;
	Wide.MaxPower = 10.f;

	AARPGPlaceholderDischarge* Halfway =
		SpawnPlaceholder<AARPGPlaceholderDischarge>(Fixture.World, Wide);
	if (!Halfway)
	{
		return false;
	}

	TestTrue(TEXT("Half of a wide window lands between the extremes"),
		Halfway->GetVolumeRadius() > Weak->GetVolumeRadius()
		&& Halfway->GetVolumeRadius() < Strong->GetVolumeRadius());

	// A burst's travel is scaled by the same number, so a charged burst reaches
	// further as well as hitting harder.
	AARPGPlaceholderDischarge* WeakBurst = SpawnPlaceholder<AARPGPlaceholderDischarge>(
		Fixture.World, MakeContext(EARPGDischargeType::Burst, Fire, 0.f));
	AARPGPlaceholderDischarge* StrongBurst = SpawnPlaceholder<AARPGPlaceholderDischarge>(
		Fixture.World, MakeContext(EARPGDischargeType::Burst, Fire, 1.f));

	if (WeakBurst && StrongBurst)
	{
		TestTrue(TEXT("A charged burst is thrown further"),
			StrongBurst->GetTravelDistance() > WeakBurst->GetTravelDistance());
	}

	// A stationary type travels nowhere, or it would drift off the caster it is
	// supposed to be centred on.
	TestEqual(TEXT("An emanation does not travel"), Strong->GetTravelDistance(), 0.f);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPlaceholderTintTest,
	"ARPG.Magic.Placeholder.PaletteReachesTheEffectFromTheSpawnPoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPlaceholderTintTest::RunTest(const FString& Parameters)
{
	using namespace ARPGPlaceholderTestUtils;

	FTestWorld Fixture;

	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire, true);
	UARPGMagicElement* Plain = MakeElement(GetTransientPackage(), TAG_Element_Water, false);

	// The tint arrives from the SPAWN POINT, the same place damage does -- an
	// effect asset should be art, not a place that re-reads the element.
	AARPGPlaceholderDischarge* Tinted = SpawnPlaceholder<AARPGPlaceholderDischarge>(
		Fixture.World, MakeContext(EARPGDischargeType::Emanate, Fire, 1.f));
	TestNotNull(TEXT("A palette-carrying element gets a placeholder"), Tinted);

	// An element with no palette has opted out of placeholders entirely, so it
	// stays invisible rather than getting a colourless stand-in that reads as a
	// finished effect.
	AddExpectedError(TEXT("no palette to build a placeholder from"),
		EAutomationExpectedErrorFlags::Contains, 0);
	TestNull(TEXT("Without a palette there is no placeholder at all"),
		Plain->ResolveDischargeEffect(EARPGDischargeType::Emanate).Get());

	// The interface is what makes one asset serve every element. Anything that
	// does not implement it is simply left alone.
	TestTrue(TEXT("The placeholder accepts a palette"),
		Tinted->Implements<UARPGElementTintable>());

	// Applying to a non-implementer, a null actor or a null element must all be
	// harmless: the spawn sites call this unconditionally.
	ARPGElementTint::Apply(nullptr, Fire);
	ARPGElementTint::Apply(Tinted, nullptr);
	AActor* Plainest = Fixture.World->SpawnActor<AActor>();
	ARPGElementTint::Apply(Plainest, Fire);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGPlaceholderDefaultsTest,
	"ARPG.Magic.Placeholder.FallbacksAreWiredWithNoConfiguration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGPlaceholderDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace ARPGPlaceholderTestUtils;

	UARPGMagicElement* Fire = MakeElement(GetTransientPackage(), TAG_Element_Fire, true);

	// A fallback system whose fallbacks have to be wired up first does not help
	// the person it exists for. Out of the box, an element with a palette and
	// nothing else authored is castable and visible.
	TestNotNull(TEXT("A burst falls back to the volume"),
		Fire->ResolveDischargeEffect(EARPGDischargeType::Burst).Get());
	TestNotNull(TEXT("A hand effect falls back"), Fire->ResolveHandEffect().Get());
	TestNotNull(TEXT("So does an imbue"), Fire->ResolveImbueEffect().Get());

	// PROJECT gets its own stand-in. Standing in for a projectile with something
	// stationary at the caster's feet would misrepresent range, travel time and
	// whether it reaches anything -- which is most of what a projectile is.
	UClass* Projectile = Fire->ResolveDischargeEffect(EARPGDischargeType::Project).Get();
	UClass* Burst = Fire->ResolveDischargeEffect(EARPGDischargeType::Burst).Get();

	TestNotNull(TEXT("A projectile falls back too"), Projectile);
	TestTrue(TEXT("And it travels rather than sitting still"),
		Projectile && Projectile->IsChildOf(AARPGPlaceholderProjectile::StaticClass()));
	TestNotEqual(TEXT("Which is a different stand-in from the stationary one"),
		Projectile, Burst);

	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGHandVisualTest,
	"ARPG.Magic.Placeholder.HandVisualFollowsWhatIsReadied",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGHandVisualTest::RunTest(const FString& Parameters)
{
	using namespace ARPGPlaceholderTestUtils;

	FTestWorld Fixture;

	AARPGCombatDummy* Caster = Fixture.World->SpawnActor<AARPGCombatDummy>();
	UAbilitySystemComponent* ASC = Caster->GetAbilitySystemComponent();
	ASC->InitAbilityActorInfo(Caster, Caster);
	ASC->SetNumericAttributeBase(UARPGVitalSet::GetMaxManaAttribute(), 100.f);
	ASC->SetNumericAttributeBase(UARPGVitalSet::GetManaAttribute(), 100.f);

	UARPGMagicLoadout* Loadout = NewObject<UARPGMagicLoadout>();
	Loadout->PageCount = 1;
	Loadout->MaxElements = 2;
	Loadout->ConformToPageCount();

	UARPGMagicElement* Fire = MakeElement(Loadout, TAG_Element_Fire, true);
	UARPGMagicElement* Water = MakeElement(Loadout, TAG_Element_Water, true);
	UARPGMagicElement* Steam = MakeElement(Loadout, TAG_Element_Steam, true);

	Loadout->SetSlot(0, EARPGElementSlot::North, Fire);
	Loadout->SetSlot(0, EARPGElementSlot::West, Water);

	UARPGMagicCombinationTable* Table = NewObject<UARPGMagicCombinationTable>();
	UARPGMagicCombinationEntry* Entry = NewObject<UARPGMagicCombinationEntry>(Table);
	Entry->RequiredElements.AddTag(TAG_Element_Fire);
	Entry->RequiredElements.AddTag(TAG_Element_Water);
	Entry->Result = Steam;
	Entry->Scope = 0xF;
	Table->Entries.Add(Entry);

	UARPGMagicComponent* Magic = NewObject<UARPGMagicComponent>(Caster);
	Magic->Loadout = Loadout;
	Magic->CombinationTable = Table;
	Magic->RegisterComponent();

	UARPGHandVisualComponent* Hand = NewObject<UARPGHandVisualComponent>(Caster);
	Hand->RegisterComponent();

	Hand->Refresh();
	TestNull(TEXT("Nothing readied, nothing in hand"), Hand->GetHandEffect());

	Magic->ToggleElement(EARPGElementSlot::North);
	Hand->Refresh();

	// The whole point: LT readying an element must be VISIBLE, or every
	// downstream behaviour is being tested blind.
	AActor* WithFire = Hand->GetHandEffect();
	TestNotNull(TEXT("Readying fire puts something in hand"), WithFire);

	// Readying a second element that COMBINES shows the combination, not two
	// motes: a combination is one thing, and it is the thing about to be cast.
	Magic->ToggleElement(EARPGElementSlot::West);
	Hand->Refresh();

	AActor* WithSteam = Hand->GetHandEffect();
	TestNotNull(TEXT("The combination is in hand"), WithSteam);
	TestNotEqual(TEXT("And it replaced the single element rather than joining it"),
		WithSteam, WithFire);

	// Refreshing an unchanged selection must not churn the actor -- this runs on
	// a tick as a safety net, so a respawn per pass would be a leak with a
	// flickering visual on top.
	Hand->Refresh();
	TestEqual(TEXT("An unchanged selection keeps the same visual"),
		Hand->GetHandEffect(), WithSteam);

	Magic->ClearSelection();
	Hand->Refresh();
	TestNull(TEXT("Discarding empties the hand"), Hand->GetHandEffect());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
