// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "ARPGCombatTypes.h"
#include "ARPGMagicTypes.h"
#include "ARPGMagicElement.generated.h"

class AActor;
class UARPGAttackDefinition;
class UARPGDamageTypeAsset;
class UARPGElementalDodgeData;
class UARPGElementPalette;
class UGameplayEffect;
class UTexture2D;

/**
 * One magic element -- either a primitive (Fire, Water) or a combination result
 * (Steam). Port of Godot's MagicElement.
 *
 * BOTH KINDS ARE THE SAME CLASS. A combination result is not a special case; it
 * is an element that happens to be produced by a table row rather than sitting
 * in a loadout slot. That is what lets fire+water->steam deal a completely
 * different type of damage with its own status, VFX and dodge, for free.
 *
 * All three consumption paths -- RT discharge, weapon imbue, elemental dodge --
 * read DamageType and OnHitEffect from whichever element is active at the moment
 * of consumption, so a combination overrides all of them at once.
 */
UCLASS(BlueprintType)
class ARPGMAGIC_API UARPGMagicElement : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	// --- Identity -------------------------------------------------------------

	/**
	 * Element.Fire, Element.Steam, and so on. Replaces Godot's string id.
	 *
	 * Combination rows, progression tracking and conduction all key off this, so
	 * two elements sharing a tag is an authoring error rather than a variant.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity",
		meta = (Categories = "Element"))
	FGameplayTag ElementTag;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FText DisplayName;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	TSoftObjectPtr<UTexture2D> Icon;

	/**
	 * Free-form classification -- "hot", "wet", "defensive", "healing". Read by
	 * the cloak (which treats a defensive element's computed damage as shield
	 * capacity instead) and by anything else wanting to branch on kind rather
	 * than on a specific element.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FGameplayTagContainer ElementTags;

	/**
	 * How many base primitives compose this element: a primitive is 1, a two-way
	 * combination is 2, and so on.
	 *
	 * Set by hand rather than derived, because the combination table is a flat
	 * lookup and not a tree -- there is nothing to walk. Drives the proficiency
	 * gate: to combine A with B, each must be mastered to at least the other's
	 * complexity.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity",
		meta = (ClampMin = "1"))
	int32 Complexity = 1;

	// --- Damage ---------------------------------------------------------------

	/** Applied by every consumption path: discharge, imbue and dodge alike. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
	TObjectPtr<UARPGDamageTypeAsset> DamageType;

	/** Scaled by the discharge's charge, type multiplier and the caster's mastery. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage",
		meta = (ClampMin = "0.0"))
	float BaseDamage = 10.f;

	/**
	 * Poise contribution. Scaled by charge and the per-type damage multiplier --
	 * both static authoring knobs -- but deliberately NOT by mastery or damage
	 * amp, which are dynamic runtime buffs. Poise stays a separate axis from
	 * HP-damage power creep, matching how melee poise damage bypasses armour.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage",
		meta = (ClampMin = "0.0"))
	float BasePoiseDamage = 0.f;

	/**
	 * How far past the blade this element reaches when it coats a weapon, as a
	 * multiple of that weapon's own hitbox radius.
	 *
	 * A coating is not painted on the edge -- a burning sword swings a sheath of
	 * hot air, and a frost one a wider chill -- so an imbued swing arms a second,
	 * larger hitbox for the element and this is how much larger. 1 keeps it
	 * exactly on the edge, which is the safe default; above that the coating
	 * starts catching things the steel misses.
	 *
	 * A multiple rather than an absolute radius so one authored number reads the
	 * same on a dagger and a greatsword.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage",
		meta = (ClampMin = "0.01"))
	float ImbueReachScale = 1.f;

	// --- Imbue ----------------------------------------------------------------

	/** Coat the next swing, or replace it. See EARPGImbueType. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Imbue")
	EARPGImbueType ImbueType = EARPGImbueType::Continuation;

	/**
	 * The attacks this element performs in place of the weapon's, one per button.
	 * Ignored unless ImbueType is SpecificAttack; any left null fall back to
	 * coating that button's ordinary swing.
	 *
	 * ON THE ELEMENT RATHER THAN IN EACH WEAPON'S TREE, which is the whole reason
	 * this is affordable. A move that belongs to the magic needs one authoring
	 * pass, not one per weapon -- and it is the right reading besides: magnetism
	 * yanking someone off their feet is not a property of the sword.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Imbue")
	TObjectPtr<UARPGAttackDefinition> ImbueAttackLight;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Imbue")
	TObjectPtr<UARPGAttackDefinition> ImbueAttackHeavy;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Imbue")
	TObjectPtr<UARPGAttackDefinition> ImbueAttackSpecial;

	/**
	 * The attack this element performs for a button, or null when it does not
	 * pre-empt that button and the ordinary swing should be coated instead.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	UARPGAttackDefinition* GetImbueAttack(EARPGAttackInput Input) const;

	/**
	 * Damage per second to anything standing inside a reservoir volume made of
	 * this element -- a lava pool.
	 *
	 * Deliberately a separate number from BaseDamage, not a reuse of it.
	 * BaseDamage is one hit's worth of impact; reusing it here would make
	 * standing in ambient water hurt every second forever, which is not what
	 * water does. 0 is right for anything whose ambient presence should carry
	 * only its status effect.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage",
		meta = (ClampMin = "0.0"))
	float AmbientDamagePerSecond = 0.f;

	// --- On hit ---------------------------------------------------------------

	/** Applied to whatever this element hits, by any path. Leave unset for pure damage. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "On Hit")
	TSubclassOf<UGameplayEffect> OnHitEffect;

	/** Overrides the effect's own duration. 0 = use the effect's. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "On Hit",
		meta = (ClampMin = "0.0"))
	float StatusDuration = 0.f;

	/**
	 * What this element LOOKS LIKE while running through a medium it conducts
	 * along: a charge crawling over a river, light down a run of glass.
	 *
	 * A gameplay effect rather than a bare effect actor because it already
	 * carries the whole vocabulary needed -- a fitted visual and a duration --
	 * so conduction adds no parallel set of VFX fields. Its combat fields go
	 * unused: this describes what conducting LOOKS like, and the charge's effect
	 * on characters is OnHitEffect above.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "On Hit")
	TSubclassOf<UGameplayEffect> ConductionEffect;

	// --- Cost -----------------------------------------------------------------

	/** Mana spent once when this element is readied. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Cost",
		meta = (ClampMin = "0.0"))
	float ActivationCost = 5.f;

	/**
	 * Multiplier on every discharge's mana cost. For a combination result this
	 * REPLACES the sum of its primitives' rates, so a combo spell is tuned on
	 * its own terms rather than inheriting whatever its ingredients cost.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Cost",
		meta = (ClampMin = "0.0"))
	float UsageRate = 1.f;

	// --- Cloak ----------------------------------------------------------------

	/** Duration at zero charge. 0 = use the cloak component's default. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Cloak",
		meta = (ClampMin = "0.0"))
	float CloakBaseDuration = 0.f;

	/** Extra seconds at full charge. 0 = a fixed-length cloak. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Cloak",
		meta = (ClampMin = "0.0"))
	float CloakChargeBonus = 0.f;

	/**
	 * Applied to the CASTER when this element's cloak activates, letting an
	 * element define self-harm (fire cloak -> Burning) or self-buff (ice cloak
	 * -> Chill). Leave unset for a neutral cloak.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Cloak")
	TSubclassOf<UGameplayEffect> CloakSelfEffect;

	// --- Dodge ----------------------------------------------------------------

	/**
	 * Everything about this element's dodge. Leave unset and the element has no
	 * dodge of its own -- consuming it falls back to the standard dodge rather
	 * than producing a broken elemental one.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Dodge")
	TObjectPtr<UARPGElementalDodgeData> ElementalDodge;

	// --- Presentation ---------------------------------------------------------

	/** Spawned at the hand socket while this element is held. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	TSoftClassPtr<AActor> HandEffect;

	/** Attached to the weapon for the duration of an imbued attack. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	TSoftClassPtr<AActor> ImbueEffect;

	/**
	 * Per-discharge-type effect actors, indexed by EARPGDischargeType.
	 *
	 * A map rather than one property per type because the type is already an
	 * enum the rest of the system switches on -- six parallel properties would
	 * need a seventh switch to read them, which is the shape the Godot version
	 * had and which is where its per-type bugs lived.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	TMap<EARPGDischargeType, TSoftClassPtr<AActor>> DischargeEffects;

	/**
	 * The few colours this element is, so the shared placeholders can stand in
	 * for anything unauthored. An element with every effect authored never
	 * consults it; an element with NO palette gets no placeholder either and
	 * stays invisible, which is the state worth warning about.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	TObjectPtr<UARPGElementPalette> Palette;

	// --- Derived --------------------------------------------------------------

	/**
	 * The effect actor for a discharge type, falling back to the shared
	 * placeholder when that slot is empty.
	 *
	 * Use this rather than reading DischargeEffects directly. An unauthored slot
	 * is the normal state during development, and a spell that spawns nothing at
	 * all is invisible in a way a grey placeholder is not: no projectile, no
	 * hitbox, no reaction, and no error anyone connects back to the spell.
	 *
	 * Returns null only when there is no placeholder to stand in either.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	TSubclassOf<AActor> ResolveDischargeEffect(EARPGDischargeType Type) const;

	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	TSubclassOf<AActor> ResolveHandEffect() const;

	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	TSubclassOf<AActor> ResolveImbueEffect() const;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGMagicElement", GetFName());
	}

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
};
