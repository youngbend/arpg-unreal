// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGMagicTypes.generated.h"

/**
 * What readying an element and then swinging actually does.
 *
 * TWO VERBS, NOT TWO TIERS. A continuation imbue changes what your attack IS
 * made of; a specific-attack imbue changes what your attack IS. Neither is the
 * upgrade -- an element that performs its own move gives up the whole combo
 * chain to do it, and one that coats the swing keeps everything the chain was
 * already building toward.
 *
 * MOST ELEMENTS ARE CONTINUATION, and that is a content decision as much as a
 * design one: a specific-attack element needs up to three authored moves before
 * it is finished, while a continuation element needs none. Reserve the type for
 * elements whose fantasy cannot be expressed as damage and status -- the ones
 * that move bodies around rather than hurting them.
 */
UENUM(BlueprintType)
enum class EARPGImbueType : uint8
{
	/**
	 * Coats the next attack and lets the chain continue. The swing is whatever
	 * the combo tree says it is; the element rides along.
	 */
	Continuation = 0,

	/**
	 * Performs one of the element's own attacks instead, chosen by which button
	 * was pressed, and ends the chain.
	 *
	 * PER BUTTON, AND PARTIAL AUTHORING IS FINE. An element that only defines a
	 * heavy falls back to continuation on light and special rather than refusing
	 * them, so a new element can ship with one move and grow.
	 */
	SpecificAttack = 1
};

/**
 * The ways an element can leave the caster's hands.
 *
 * Port of Godot's DischargeContext::DischargeType, and the values matter: they
 * index the per-type VFX slots on an element and the per-type cost and damage
 * knobs on the magic component, so reordering them silently re-points authored
 * content.
 */
UENUM(BlueprintType)
enum class EARPGDischargeType : uint8
{
	/** Weapon imbue -- applied when an imbued strike lands. Not chargeable. */
	Imbuement = 0,
	/** RT+B: coats the caster in the element. */
	Cloak = 1,
	/** RT+X: close-range burst forward. */
	Burst = 2,
	/** RT+A: emanation around the caster. */
	Emanate = 3,
	/** RT+Y: mid-range projectile. */
	Project = 4,
	/**
	 * Released at a contact point rather than thrown by a caster -- what a
	 * reaction product looks like. Distinct from Burst for a real reason: a
	 * burst is a cone aimed by the caster, a collision is omnidirectional from
	 * wherever the two volumes touched.
	 */
	Collision = 5,

	MAX UMETA(Hidden)
};

/**
 * Where a combination row applies. A bitmask, so a row that holds everywhere is
 * authored once rather than duplicated.
 *
 * WHY SCOPE EXISTS AT ALL. Hand combination is the caster SYNTHESISING a new
 * element out of two they hold -- a recipe, answerable to the magic system's own
 * logic. A world interaction is two things meeting and obeying physics. Air plus
 * water may well be ice in a caster's hands; an air blast over a lake is just
 * wind over water and should certainly not freeze it. Conversely ice striking a
 * lake should freeze it, which is meaningless as a hand recipe.
 */
UENUM(BlueprintType, meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class EARPGCombinationScope : uint8
{
	None = 0,
	/** The caster fusing two held elements. */
	Hand = 1,
	/** Two things meeting in the world, conduction included. */
	Collision = 2,
	/** Two media sharing ground, resolved per tick. */
	Field = 4,
	/**
	 * Something meeting a BODY OF FLUID lying on the ground. Distinct from
	 * Collision for the same reason scope exists: an ice shard meeting a water
	 * JET is two spells trading energy, while the same shard meeting a PUDDLE
	 * freezes part of its surface into something you can stand on.
	 */
	Surface = 8
};
ENUM_CLASS_FLAGS(EARPGCombinationScope);

/** Everything that is not the caster's own hands. */
#define ARPG_COMBINATION_SCOPE_WORLD \
	(EARPGCombinationScope::Collision | EARPGCombinationScope::Field | EARPGCombinationScope::Surface)

/**
 * What a matched combination row actually does to its reactants.
 *
 * AMPLIFICATION needs no entry here: when the result IS one of the reactants,
 * that is not a transmutation but one element feeding the other. Fire + air ->
 * fire makes a fireball eat an air projectile and grow, and makes wind fan a
 * grass fire, from one row.
 */
UENUM(BlueprintType)
enum class EARPGReactionMode : uint8
{
	/** Both reactants are consumed and the result is produced. The usual case. */
	Auto,
	/**
	 * The result TRAVELS THROUGH the other reactant rather than reacting with
	 * it -- lightning through water, light through glass. The medium is a
	 * carrier, not a reactant: it is not consumed, and what arrives at the far
	 * end is still the element that entered.
	 *
	 * The one relationship that could not be derived from the others, which is
	 * why it is declared rather than inferred.
	 */
	Conduct,
	/** The result is left behind as a standable solid -- ice over a puddle. */
	Solidify
};
