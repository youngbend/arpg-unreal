// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ARPGArmorComponent.h"
#include "ARPGArmorDefinition.h"
#include "ARPGAttackDefinition.h"
#include "ARPGCombatDummy.h"
#include "ARPGComboComponent.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGGameplayTags.h"
#include "ARPGHitboxComponent.h"
#include "ARPGOffenseSet.h"
#include "ARPGResistanceSet.h"
#include "ARPGWeaponAttackTree.h"
#include "ARPGWeaponComponent.h"
#include "ARPGWeaponDefinition.h"
#include "AbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

/**
 * Weapons and armour.
 *
 * The behaviour worth protecting here is not "equipping raises a number" but
 * that UNEQUIPPING PUTS IT BACK EXACTLY. Both components apply their
 * contribution to attributes on equip, so any asymmetry between the apply and
 * remove paths shows up as a character who quietly gets tankier every time they
 * swap kit -- a drift that is invisible in a single session and ruinous over a
 * long one. Every case below equips, swaps and unequips, then asserts the
 * attribute is back at its starting value.
 */
namespace ARPGEquipmentTestUtils
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

	UARPGDamageTypeAsset* MakeDamageType(const FGameplayAttribute& ResistanceAttribute)
	{
		UARPGDamageTypeAsset* Type = NewObject<UARPGDamageTypeAsset>();
		Type->ResistanceAttribute = ResistanceAttribute;
		return Type;
	}

	struct FRig
	{
		AARPGCombatDummy* Actor = nullptr;
		UARPGWeaponComponent* WeaponComp = nullptr;
		UARPGArmorComponent* ArmorComp = nullptr;
		UARPGComboComponent* Combo = nullptr;
		UARPGHitboxComponent* Hitbox = nullptr;

		float Attr(const FGameplayAttribute& Attribute) const
		{
			return Actor->GetAbilitySystemComponent()->GetNumericAttribute(Attribute);
		}

		float Armor() const { return Attr(UARPGResistanceSet::GetBaseArmorAttribute()); }
		float Crit() const { return Attr(UARPGOffenseSet::GetCritChanceAttribute()); }
		float FireRes() const { return Attr(UARPGResistanceSet::GetFireResistanceAttribute()); }
		float IceRes() const { return Attr(UARPGResistanceSet::GetIceResistanceAttribute()); }
	};

	FRig BuildRig(UWorld* World)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		FRig Rig;
		Rig.Actor = World->SpawnActor<AARPGCombatDummy>(
			AARPGCombatDummy::StaticClass(), FTransform::Identity, Params);

		UAbilitySystemComponent* ASC = Rig.Actor->GetAbilitySystemComponent();
		ASC->InitAbilityActorInfo(Rig.Actor, Rig.Actor);

		Rig.Combo = NewObject<UARPGComboComponent>(Rig.Actor);
		Rig.Combo->RegisterComponent();

		Rig.Hitbox = NewObject<UARPGHitboxComponent>(Rig.Actor);
		Rig.Hitbox->HitboxSource = EARPGHitboxSource::Weapon;
		Rig.Hitbox->RegisterComponent();

		Rig.WeaponComp = NewObject<UARPGWeaponComponent>(Rig.Actor);
		Rig.WeaponComp->RegisterComponent();

		Rig.ArmorComp = NewObject<UARPGArmorComponent>(Rig.Actor);
		Rig.ArmorComp->RegisterComponent();

		return Rig;
	}

	UARPGWeaponAttackTree* MakeTree(UObject* Outer, const TCHAR* AttackId)
	{
		UARPGWeaponAttackTree* Tree = NewObject<UARPGWeaponAttackTree>(Outer);
		UARPGAttackDefinition* Attack = NewObject<UARPGAttackDefinition>(Tree);
		Attack->AttackId = AttackId;

		UARPGComboAttackNode* Root = NewObject<UARPGComboAttackNode>(Tree);
		Root->Attack = Attack;
		Tree->RootLight = Root;
		return Tree;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGWeaponEquipTest,
	"ARPG.Combat.Equipment.WeaponEquipRewiresMoveset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGWeaponEquipTest::RunTest(const FString& Parameters)
{
	using namespace ARPGEquipmentTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	TestNull(TEXT("Setup: no moveset before anything is equipped"), Rig.Combo->AttackTree.Get());

	UARPGWeaponDefinition* Sword = NewObject<UARPGWeaponDefinition>();
	Sword->WeaponId = TEXT("sword");
	Sword->BaseDamage = 30.f;
	Sword->AttackTree = MakeTree(Sword, TEXT("sword_light"));

	UARPGWeaponDefinition* Axe = NewObject<UARPGWeaponDefinition>();
	Axe->WeaponId = TEXT("axe");
	Axe->BaseDamage = 55.f;
	Axe->AttackTree = MakeTree(Axe, TEXT("axe_light"));

	Rig.WeaponComp->EquipWeapon(Sword);
	TestSamePtr(TEXT("Sword's tree drives the combo"), Rig.Combo->AttackTree.Get(), Sword->AttackTree.Get());
	TestEqual(TEXT("Sword's damage reaches the hitbox"), Rig.Hitbox->WeaponBaseDamage, 30.f);

	// The point of the whole component: swapping weapons swaps what the attack
	// buttons do, without the character knowing anything about either weapon.
	Rig.WeaponComp->EquipWeapon(Axe);
	TestSamePtr(TEXT("Axe's tree replaces it"), Rig.Combo->AttackTree.Get(), Axe->AttackTree.Get());
	TestEqual(TEXT("Axe's damage replaces it"), Rig.Hitbox->WeaponBaseDamage, 55.f);

	Rig.WeaponComp->UnequipWeapon();
	TestNull(TEXT("Unarmed has no moveset"), Rig.Combo->AttackTree.Get());
	TestEqual(TEXT("Unarmed deals no weapon damage"), Rig.Hitbox->WeaponBaseDamage, 0.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGUnarmedMovesetTest,
	"ARPG.Combat.Equipment.UnarmedKeepsNoWeaponMoveset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * The regression: sword combos with nothing in hand.
 *
 * The cause was one line of character setup -- the sword's tree seeded into the
 * combo component's FALLBACK slot, which is the moveset for bare hands -- so
 * unequipping the sword took nothing away. Nothing in the components could have
 * caught it, because both fields were being used exactly as designed. What is
 * asserted here is the invariant that makes that misuse visible: what a
 * character can do with empty hands never comes from a weapon.
 */
bool FARPGUnarmedMovesetTest::RunTest(const FString& Parameters)
{
	using namespace ARPGEquipmentTestUtils;
	FTestWorld Scope;

	{
		FRig Rig = BuildRig(Scope.World);

		// Begin play with nothing equipped has to PUSH the unarmed state, not
		// merely decline to write a weapon's: the hitbox ships with an authored
		// WeaponBaseDamage, and a character who never equips anything would
		// otherwise punch for a sword's worth of it.
		TestEqual(TEXT("Unarmed at begin play swings for nothing"),
			Rig.Hitbox->WeaponBaseDamage, 0.f);

		UARPGWeaponAttackTree* Fists = MakeTree(Rig.Actor, TEXT("jab"));
		Rig.Combo->FallbackAttackTree = Fists;

		UARPGWeaponDefinition* Sword = NewObject<UARPGWeaponDefinition>();
		Sword->WeaponId = TEXT("sword");
		Sword->BaseDamage = 30.f;
		Sword->AttackTree = MakeTree(Sword, TEXT("sword_light"));

		Rig.WeaponComp->EquipWeapon(Sword);
		TestSamePtr(TEXT("Armed, the weapon's moveset takes over from the unarmed one"),
			Rig.Combo->GetActiveTree(), Sword->AttackTree.Get());

		Rig.WeaponComp->UnequipWeapon();
		TestSamePtr(TEXT("Unarmed goes back to the unarmed moveset, not the sword's"),
			Rig.Combo->GetActiveTree(), Fists);
		TestEqual(TEXT("...and back to unarmed damage with it"),
			Rig.Hitbox->WeaponBaseDamage, 0.f);

		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		const UARPGComboAttackNode* Node = Rig.Combo->GetCurrentNode();
		TestEqual(TEXT("...and the beat a press starts is the unarmed one"),
			Node && Node->Attack ? Node->Attack->AttackId.ToString() : FString(TEXT("<none>")),
			FString(TEXT("jab")));
	}

	// No unarmed moveset authored at all -- the shipping default. Empty hands
	// then do nothing, which is the honest answer and the one the previous
	// behaviour hid by handing them a sword.
	{
		FRig Rig = BuildRig(Scope.World);

		Rig.Combo->ReceiveInput(EARPGAttackInput::Light);
		TestFalse(TEXT("With no unarmed moveset, a bare-handed press starts nothing"),
			Rig.Combo->IsAttacking());
		TestNull(TEXT("...and leaves the chain at rest"), Rig.Combo->GetCurrentNode());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGWeaponCritTest,
	"ARPG.Combat.Equipment.WeaponCritIsSymmetric",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGWeaponCritTest::RunTest(const FString& Parameters)
{
	using namespace ARPGEquipmentTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UAbilitySystemComponent* ASC = Rig.Actor->GetAbilitySystemComponent();
	ASC->SetNumericAttributeBase(UARPGOffenseSet::GetCritChanceAttribute(), 0.05f);
	TestEqual(TEXT("Setup: character's own crit chance"), Rig.Crit(), 0.05f);

	FARPGWeaponModifier Keen;
	Keen.CritChanceBonus = 0.2f;

	UARPGWeaponDefinition* Dagger = NewObject<UARPGWeaponDefinition>();
	Dagger->WeaponId = TEXT("dagger");
	Dagger->Modifiers.Add(Keen);
	Dagger->AttackTree = MakeTree(Dagger, TEXT("dagger_light")); // else it warns, correctly

	Rig.WeaponComp->EquipWeapon(Dagger);
	TestEqual(TEXT("Weapon's affix adds to the wielder's crit"), Rig.Crit(), 0.25f);

	// The regression this exists for: a bonus applied on equip but recomputed
	// (or forgotten) on unequip leaves the character permanently keener.
	Rig.WeaponComp->UnequipWeapon();
	TestEqual(TEXT("Unequipping restores the base crit exactly"), Rig.Crit(), 0.05f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGArmorEquipTest,
	"ARPG.Combat.Equipment.ArmorAppliesAndReverses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGArmorEquipTest::RunTest(const FString& Parameters)
{
	using namespace ARPGEquipmentTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UAbilitySystemComponent* ASC = Rig.Actor->GetAbilitySystemComponent();
	ASC->SetNumericAttributeBase(UARPGResistanceSet::GetBaseArmorAttribute(), 10.f);
	ASC->SetNumericAttributeBase(UARPGResistanceSet::GetFireResistanceAttribute(), 0.1f);
	TestEqual(TEXT("Setup: innate armour"), Rig.Armor(), 10.f);
	TestEqual(TEXT("Setup: innate fire resistance"), Rig.FireRes(), 0.1f);

	UARPGDamageTypeAsset* Fire = MakeDamageType(UARPGResistanceSet::GetFireResistanceAttribute());
	UARPGDamageTypeAsset* Ice  = MakeDamageType(UARPGResistanceSet::GetIceResistanceAttribute());

	// Two modifiers naming the same type, to prove they sum rather than the last
	// one winning -- affixes stack on a piece.
	FARPGArmorModifier Warded;
	Warded.FlatArmorBonus = 5.f;
	Warded.ResistanceBonuses.Add(Fire, 0.15f);

	FARPGArmorModifier Quenched;
	Quenched.ResistanceBonuses.Add(Fire, 0.05f);
	Quenched.ResistanceBonuses.Add(Ice, -0.1f); // vulnerability: bonuses may be negative

	UARPGArmorDefinition* Plate = NewObject<UARPGArmorDefinition>();
	Plate->ArmorId = TEXT("plate");
	Plate->ArmorType = EARPGArmorType::Heavy;
	Plate->ArmorValue = 40.f;
	Plate->Modifiers.Add(Warded);
	Plate->Modifiers.Add(Quenched);

	Rig.ArmorComp->EquipArmor(Plate);
	TestEqual(TEXT("Flat armour adds to the wearer's own"), Rig.Armor(), 55.f);
	TestEqual(TEXT("Resistance affixes sum across modifiers"), Rig.FireRes(), 0.3f);
	TestEqual(TEXT("Negative bonuses apply as vulnerability"), Rig.IceRes(), -0.1f);
	TestEqual(TEXT("Progression key follows the type"), Rig.ArmorComp->GetArmorTypeName(), FName(TEXT("heavy")));

	UARPGArmorDefinition* Robe = NewObject<UARPGArmorDefinition>();
	Robe->ArmorId = TEXT("robe");
	Robe->ArmorType = EARPGArmorType::Light;
	Robe->ArmorValue = 8.f;

	// Swapping must remove ALL of the plate, including the ice vulnerability the
	// robe says nothing about.
	Rig.ArmorComp->EquipArmor(Robe);
	TestEqual(TEXT("Swap replaces flat armour"), Rig.Armor(), 18.f);
	TestEqual(TEXT("Swap drops the old fire resistance"), Rig.FireRes(), 0.1f);
	TestEqual(TEXT("Swap drops the old ice vulnerability"), Rig.IceRes(), 0.f);

	Rig.ArmorComp->UnequipArmor();
	TestEqual(TEXT("Unequipping restores innate armour exactly"), Rig.Armor(), 10.f);
	TestEqual(TEXT("Unequipping restores innate fire resistance exactly"), Rig.FireRes(), 0.1f);
	TestEqual(TEXT("Unarmoured reports as such"), Rig.ArmorComp->GetArmorTypeName(), FName(TEXT("unarmored")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGArmorEditedWhileWornTest,
	"ARPG.Combat.Equipment.ArmorEditedWhileWornStillReverses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGArmorEditedWhileWornTest::RunTest(const FString& Parameters)
{
	using namespace ARPGEquipmentTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UAbilitySystemComponent* ASC = Rig.Actor->GetAbilitySystemComponent();
	ASC->SetNumericAttributeBase(UARPGResistanceSet::GetBaseArmorAttribute(), 10.f);

	UARPGArmorDefinition* Plate = NewObject<UARPGArmorDefinition>();
	Plate->ArmorId = TEXT("plate");
	Plate->ArmorValue = 40.f;

	Rig.ArmorComp->EquipArmor(Plate);
	TestEqual(TEXT("Setup: armour applied"), Rig.Armor(), 50.f);

	// The reason the component records what it applied instead of recomputing it
	// from the definition: a reroll, an enchant, or a designer editing the asset
	// in a live editor session all change the definition while it is worn. A
	// remove path that recomputed would subtract 100 here and leave the wearer
	// worse off than they started.
	Plate->ArmorValue = 100.f;

	Rig.ArmorComp->UnequipArmor();
	TestEqual(TEXT("Removal reverses what was applied, not what the asset now says"),
		Rig.Armor(), 10.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGArmorUnmappedResistanceTest,
	"ARPG.Combat.Equipment.ArmorWarnsOnUnmappedResistance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FARPGArmorUnmappedResistanceTest::RunTest(const FString& Parameters)
{
	using namespace ARPGEquipmentTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UAbilitySystemComponent* ASC = Rig.Actor->GetAbilitySystemComponent();
	ASC->SetNumericAttributeBase(UARPGResistanceSet::GetBaseArmorAttribute(), 10.f);

	// A damage type with no ResistanceAttribute set -- the authoring mistake that
	// would otherwise make an armour piece silently do nothing against it.
	UARPGDamageTypeAsset* Unmapped = NewObject<UARPGDamageTypeAsset>();

	FARPGArmorModifier Modifier;
	Modifier.ResistanceBonuses.Add(Unmapped, 0.5f);

	UARPGArmorDefinition* Piece = NewObject<UARPGArmorDefinition>();
	Piece->ArmorId = TEXT("mystery");
	Piece->Modifiers.Add(Modifier);

	AddExpectedError(TEXT("has no .*ResistanceAttribute"), EAutomationExpectedErrorFlags::Contains, 1);
	Rig.ArmorComp->EquipArmor(Piece);

	// And it must still unequip cleanly rather than leaving a half-applied state.
	Rig.ArmorComp->UnequipArmor();
	TestEqual(TEXT("Nothing was applied, nothing is left behind"), Rig.Armor(), 10.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGEquipmentBuffDriftTest,
	"ARPG.Combat.Equipment.GrantsDoNotAbsorbLiveBuffs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * A worn piece must not swallow a buff that happens to be live when it is
 * equipped.
 *
 * THE BUG THIS PINS DOWN. Both equipment components used to read the CURRENT
 * value of an attribute -- base plus every active modifier -- and write it back
 * as the BASE. Equipping armour while any resistance buff was running therefore
 * folded that buff permanently into the base, and it stayed after the buff
 * expired and after the armour came off. Repeat the cycle and a character
 * ratchets upward without limit.
 *
 * The grant is a GameplayEffect now, so the aggregator owns it and removal is
 * exact. This asserts the property the old shape could not have: the base is
 * untouched throughout, and every combination of buff-on/off and armour-on/off
 * lands on the arithmetic sum.
 */
bool FARPGEquipmentBuffDriftTest::RunTest(const FString& Parameters)
{
	using namespace ARPGEquipmentTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UAbilitySystemComponent* ASC = Rig.Actor->GetAbilitySystemComponent();
	const FGameplayAttribute FireAttr = UARPGResistanceSet::GetFireResistanceAttribute();

	ASC->SetNumericAttributeBase(FireAttr, 0.1f);
	TestEqual(TEXT("Setup: innate fire resistance"), Rig.FireRes(), 0.1f);

	// A live buff, exactly as a Wet status would apply one.
	UGameplayEffect* Buff = NewObject<UGameplayEffect>(GetTransientPackage(), TEXT("FireResBuff"));
	Buff->DurationPolicy = EGameplayEffectDurationType::Infinite;
	{
		FGameplayModifierInfo Mod;
		Mod.Attribute = FireAttr;
		Mod.ModifierOp = EGameplayModOp::Additive;
		Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(0.2f));
		Buff->Modifiers.Add(Mod);
	}

	const FActiveGameplayEffectHandle BuffHandle =
		ASC->ApplyGameplayEffectToSelf(Buff, 1.f, ASC->MakeEffectContext());

	TestEqual(TEXT("Buff raises the current value"), Rig.FireRes(), 0.3f);

	UARPGDamageTypeAsset* Fire = MakeDamageType(FireAttr);

	FARPGArmorModifier Warded;
	Warded.ResistanceBonuses.Add(Fire, 0.25f);

	UARPGArmorDefinition* Piece = NewObject<UARPGArmorDefinition>();
	Piece->ArmorId = TEXT("warded");
	Piece->Modifiers.Add(Warded);

	// Equipped WHILE the buff is live -- the moment the old shape went wrong.
	Rig.ArmorComp->EquipArmor(Piece);
	TestEqual(TEXT("Armour composes with the buff"), Rig.FireRes(), 0.55f);
	TestEqual(TEXT("And the BASE is untouched"),
		ASC->GetNumericAttributeBase(FireAttr), 0.1f);

	// The buff ends. What is left must be innate plus armour, with none of the
	// buff baked in.
	ASC->RemoveActiveGameplayEffect(BuffHandle);
	TestEqual(TEXT("Buff expiring leaves innate + armour only"), Rig.FireRes(), 0.35f);

	Rig.ArmorComp->UnequipArmor();
	TestEqual(TEXT("Unequipping returns exactly to innate"), Rig.FireRes(), 0.1f);
	TestEqual(TEXT("Base never moved at all"),
		ASC->GetNumericAttributeBase(FireAttr), 0.1f);

	// The ratchet: repeating the cycle must not accumulate anything.
	for (int32 Cycle = 0; Cycle < 3; ++Cycle)
	{
		Rig.ArmorComp->EquipArmor(Piece);
		Rig.ArmorComp->UnequipArmor();
	}
	TestEqual(TEXT("Three equip/unequip cycles leave no drift"), Rig.FireRes(), 0.1f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FARPGWeaponCritDriftTest,
	"ARPG.Combat.Equipment.WeaponCritGrantDoesNotDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** The same property for the weapon's crit bonus, which had the same shape. */
bool FARPGWeaponCritDriftTest::RunTest(const FString& Parameters)
{
	using namespace ARPGEquipmentTestUtils;
	FTestWorld Scope;
	FRig Rig = BuildRig(Scope.World);

	UAbilitySystemComponent* ASC = Rig.Actor->GetAbilitySystemComponent();
	const FGameplayAttribute CritAttr = UARPGOffenseSet::GetCritChanceAttribute();

	ASC->SetNumericAttributeBase(CritAttr, 0.05f);

	UGameplayEffect* Buff = NewObject<UGameplayEffect>(GetTransientPackage(), TEXT("CritBuff"));
	Buff->DurationPolicy = EGameplayEffectDurationType::Infinite;
	{
		FGameplayModifierInfo Mod;
		Mod.Attribute = CritAttr;
		Mod.ModifierOp = EGameplayModOp::Additive;
		Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(0.1f));
		Buff->Modifiers.Add(Mod);
	}
	const FActiveGameplayEffectHandle BuffHandle =
		ASC->ApplyGameplayEffectToSelf(Buff, 1.f, ASC->MakeEffectContext());

	FARPGWeaponModifier Keen;
	Keen.CritChanceBonus = 0.2f;

	UARPGWeaponDefinition* Dagger = NewObject<UARPGWeaponDefinition>();
	Dagger->WeaponId = TEXT("keen_dagger");
	Dagger->AttackTree = MakeTree(Dagger, TEXT("dagger_light"));
	Dagger->Modifiers.Add(Keen);

	Rig.WeaponComp->EquipWeapon(Dagger);
	TestEqual(TEXT("Weapon crit composes with the buff"), Rig.Crit(), 0.35f);

	ASC->RemoveActiveGameplayEffect(BuffHandle);
	TestEqual(TEXT("Buff expiring leaves innate + weapon"), Rig.Crit(), 0.25f);

	Rig.WeaponComp->UnequipWeapon();
	TestEqual(TEXT("Unequipping returns exactly to innate"), Rig.Crit(), 0.05f);
	TestEqual(TEXT("Base never moved"), ASC->GetNumericAttributeBase(CritAttr), 0.05f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
