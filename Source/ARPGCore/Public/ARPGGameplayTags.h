// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "NativeGameplayTags.h"

/**
 * Native gameplay tags for the whole game.
 *
 * These replace the String ids the Godot project keyed everything on --
 * DamageType::id, MagicElement::id, StatusEffectDefinition::id,
 * CharacterBase::Faction -- with a typed, hierarchical, editor-pickable
 * vocabulary. The names below deliberately mirror the shipped .tres ids
 * one-for-one so the two projects stay comparable during the port.
 *
 * Declared with the module API macro in front of the declare macro so the
 * symbol is genuinely dll-exported: UE_DECLARE_GAMEPLAY_TAG_EXTERN expands to a
 * bare `extern FNativeGameplayTag`, which does not link across a module
 * boundary in a modular (editor) build on its own.
 */

// -----------------------------------------------------------------------------
// Damage types  (project/combat/damage_types/*.tres)
// -----------------------------------------------------------------------------
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Damage_Physical);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Damage_Fire);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Damage_Ice);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Damage_Light);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Damage_Lightning);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Damage_Water);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Damage_Fall);

// Categories -- the CATEGORY_* bitfield on DamageType becomes a tag container.
// TAG_Damage_Category_True still ignores all resistance; Healing still makes
// receive_damage() treat the magnitude as heal power and skip mitigation.
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Damage_Category_Physical);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Damage_Category_Magical);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Damage_Category_Elemental);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Damage_Category_True);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Damage_Category_Healing);

// -----------------------------------------------------------------------------
// Magic elements  (project/magic/**/*.tres)
// -----------------------------------------------------------------------------
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Element_Air);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Element_Earth);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Element_Fire);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Element_Lightning);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Element_Water);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Element_Ice);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Element_Steam);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Element_Lava);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Element_Obsidian);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Element_ConductedLightning);

// -----------------------------------------------------------------------------
// Progression subcomponents
//
// Weapon and armour TYPES, as the ids progression tracks proficiency against.
// Tags rather than the enums they mirror because a tracker keys every category
// the same way -- Element.Fire, Weapon.Sword and Armor.Heavy are all just "which
// thing did you use" -- and one keying scheme means one tracker rather than
// three that differ only in their map type.
// -----------------------------------------------------------------------------
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Weapon_Unarmed);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Weapon_Sword);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Weapon_Axe);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Weapon_Mace);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Weapon_Spear);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Weapon_Dagger);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Weapon_Greatsword);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Weapon_Staff);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Weapon_Wand);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Weapon_Orb);

ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Armor_Unarmored);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Armor_Light);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Armor_Medium);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Armor_Heavy);

// -----------------------------------------------------------------------------
// Status effects  (project/combat/status_effects/*.tres)
// -----------------------------------------------------------------------------
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Status_Burning);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Status_Shocked);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Status_Weakened);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Status_Wet);

// -----------------------------------------------------------------------------
// Factions  (CharacterBase::Faction)
// -----------------------------------------------------------------------------
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Faction_Player);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Faction_Ally);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Faction_Enemy);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Faction_Neutral);

// -----------------------------------------------------------------------------
// Character state
//
// These carry the plumbing that was manual in Godot. State.Hyperarmor replaces
// PoiseComponent::set_attack_hyperarmor() being fed every frame by animation.gd;
// State.Flinching replaces ComboComponent::set_attack_locked(). Both become
// ActivationBlockedTags on the relevant abilities.
// -----------------------------------------------------------------------------
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_Dead);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_Attacking);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_Charging);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_Channeling);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_Hyperarmor);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_Flinching);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_Flinching_Light);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_Flinching_Heavy);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_StanceBroken);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_Blocking);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_Parrying);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_Dodging);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_Invulnerable);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_HitStop);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_WeaponDrawn);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_Cloaked);

// -----------------------------------------------------------------------------
// Ability identity  (used for CancelAbilitiesWithTag / BlockAbilitiesWithTag)
// -----------------------------------------------------------------------------
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Attack);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Attack_Melee);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Attack_Air);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Dodge);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Block);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Parry);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Discharge);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Discharge_Burst);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Discharge_Emanate);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Discharge_Project);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Discharge_Cloak);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Discharge_Collision);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Imbue);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Consumable);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Flinch);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_StanceBreak);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Death);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_DrawSheathe);

// -----------------------------------------------------------------------------
// Gameplay events
//
// Event.Attack.* are what ComboComponent and the anim notifies raise; the
// ability system routes them to the triggered ability. Event.Poise.* replace
// the light_flinched / heavy_flinched / stance_broke signals.
// -----------------------------------------------------------------------------
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Attack_Begin);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Attack_ComboWindow);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Attack_HitboxOn);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Attack_HitboxOff);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Attack_ChargeRelease);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Attack_ChannelStop);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Poise_LightFlinch);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Poise_HeavyFlinch);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Poise_StanceBreak);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Death);

// -----------------------------------------------------------------------------
// SetByCaller magnitudes
// -----------------------------------------------------------------------------
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Data_Damage);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Data_Healing);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Data_PoiseDamage);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Data_ManaCost);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Data_StaminaCost);

// -----------------------------------------------------------------------------
// Equipment stat grants
//
// One SetByCaller per attribute a worn piece can move, read by
// UARPGEquipmentGameplayEffect. A tag rather than a dynamically-built effect
// because a runtime-constructed UGameplayEffect has no network identity: its
// definition pointer cannot be resolved on a client, so the owning player's
// predicted view of their own equipment would arrive with a null Def.
// -----------------------------------------------------------------------------
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Data_Equipment_Armor);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Data_Equipment_CritChance);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Data_Equipment_Resistance_Physical);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Data_Equipment_Resistance_Fire);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Data_Equipment_Resistance_Ice);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Data_Equipment_Resistance_Light);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Data_Equipment_Resistance_Lightning);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Data_Equipment_Resistance_Water);
ARPGCORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Data_Equipment_Resistance_Fall);
