// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGGameplayTags.h"

// Damage types.
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Physical,  "Damage.Physical");
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Fire,      "Damage.Fire");
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Ice,       "Damage.Ice");
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Light,     "Damage.Light");
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Lightning, "Damage.Lightning");
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Water,     "Damage.Water");
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Fall,      "Damage.Fall");

UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Category_Physical,  "Damage.Category.Physical");
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Category_Magical,   "Damage.Category.Magical");
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Category_Elemental, "Damage.Category.Elemental");
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Category_True,      "Damage.Category.True");
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Category_Healing,   "Damage.Category.Healing");

// Magic elements.
UE_DEFINE_GAMEPLAY_TAG(TAG_Element_Air,                "Element.Air");
UE_DEFINE_GAMEPLAY_TAG(TAG_Element_Earth,              "Element.Earth");
UE_DEFINE_GAMEPLAY_TAG(TAG_Element_Fire,               "Element.Fire");
UE_DEFINE_GAMEPLAY_TAG(TAG_Element_Lightning,          "Element.Lightning");
UE_DEFINE_GAMEPLAY_TAG(TAG_Element_Water,              "Element.Water");
UE_DEFINE_GAMEPLAY_TAG(TAG_Element_Ice,                "Element.Ice");
UE_DEFINE_GAMEPLAY_TAG(TAG_Element_Steam,              "Element.Steam");
UE_DEFINE_GAMEPLAY_TAG(TAG_Element_Lava,               "Element.Lava");
UE_DEFINE_GAMEPLAY_TAG(TAG_Element_Obsidian,           "Element.Obsidian");
UE_DEFINE_GAMEPLAY_TAG(TAG_Element_ConductedLightning, "Element.ConductedLightning");

// Progression subcomponents.
UE_DEFINE_GAMEPLAY_TAG(TAG_Weapon_Unarmed,             "Weapon.Unarmed");
UE_DEFINE_GAMEPLAY_TAG(TAG_Weapon_Sword,               "Weapon.Sword");
UE_DEFINE_GAMEPLAY_TAG(TAG_Weapon_Axe,                 "Weapon.Axe");
UE_DEFINE_GAMEPLAY_TAG(TAG_Weapon_Mace,                "Weapon.Mace");
UE_DEFINE_GAMEPLAY_TAG(TAG_Weapon_Spear,               "Weapon.Spear");
UE_DEFINE_GAMEPLAY_TAG(TAG_Weapon_Dagger,              "Weapon.Dagger");
UE_DEFINE_GAMEPLAY_TAG(TAG_Weapon_Greatsword,          "Weapon.Greatsword");
UE_DEFINE_GAMEPLAY_TAG(TAG_Weapon_Staff,               "Weapon.Staff");
UE_DEFINE_GAMEPLAY_TAG(TAG_Weapon_Wand,                "Weapon.Wand");
UE_DEFINE_GAMEPLAY_TAG(TAG_Weapon_Orb,                 "Weapon.Orb");

UE_DEFINE_GAMEPLAY_TAG(TAG_Armor_Unarmored,            "Armor.Unarmored");
UE_DEFINE_GAMEPLAY_TAG(TAG_Armor_Light,                "Armor.Light");
UE_DEFINE_GAMEPLAY_TAG(TAG_Armor_Medium,               "Armor.Medium");
UE_DEFINE_GAMEPLAY_TAG(TAG_Armor_Heavy,                "Armor.Heavy");

// Status effects.
UE_DEFINE_GAMEPLAY_TAG(TAG_Status_Burning,  "Status.Burning");
UE_DEFINE_GAMEPLAY_TAG(TAG_Status_Shocked,  "Status.Shocked");
UE_DEFINE_GAMEPLAY_TAG(TAG_Status_Weakened, "Status.Weakened");
UE_DEFINE_GAMEPLAY_TAG(TAG_Status_Wet,      "Status.Wet");

// Factions.
UE_DEFINE_GAMEPLAY_TAG(TAG_Faction_Player,  "Faction.Player");
UE_DEFINE_GAMEPLAY_TAG(TAG_Faction_Ally,    "Faction.Ally");
UE_DEFINE_GAMEPLAY_TAG(TAG_Faction_Enemy,   "Faction.Enemy");
UE_DEFINE_GAMEPLAY_TAG(TAG_Faction_Neutral, "Faction.Neutral");

// Character state.
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Dead,            "State.Dead");
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Attacking,       "State.Attacking");
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Charging,        "State.Charging");
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Channeling,      "State.Channeling");
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Hyperarmor,      "State.Hyperarmor");
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Flinching,       "State.Flinching");
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Flinching_Light, "State.Flinching.Light");
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Flinching_Heavy, "State.Flinching.Heavy");
UE_DEFINE_GAMEPLAY_TAG(TAG_State_StanceBroken,    "State.StanceBroken");
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Blocking,        "State.Blocking");
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Parrying,        "State.Parrying");
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Dodging,         "State.Dodging");
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Invulnerable,    "State.Invulnerable");
UE_DEFINE_GAMEPLAY_TAG(TAG_State_HitStop,         "State.HitStop");
UE_DEFINE_GAMEPLAY_TAG(TAG_State_WeaponDrawn,     "State.WeaponDrawn");
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Cloaked,         "State.Cloaked");

// Ability identity.
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Attack,               "Ability.Attack");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Attack_Melee,         "Ability.Attack.Melee");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Attack_Air,           "Ability.Attack.Air");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Dodge,                "Ability.Dodge");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Block,                "Ability.Block");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Parry,                "Ability.Parry");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Discharge,            "Ability.Discharge");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Discharge_Burst,      "Ability.Discharge.Burst");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Discharge_Emanate,    "Ability.Discharge.Emanate");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Discharge_Project,    "Ability.Discharge.Project");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Discharge_Cloak,      "Ability.Discharge.Cloak");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Discharge_Collision,  "Ability.Discharge.Collision");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Imbue,                "Ability.Imbue");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Consumable,           "Ability.Consumable");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Flinch,               "Ability.Flinch");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_StanceBreak,          "Ability.StanceBreak");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Death,                "Ability.Death");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_DrawSheathe,          "Ability.DrawSheathe");

// Gameplay events.
UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Attack_Begin,         "Event.Attack.Begin");
UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Attack_ComboWindow,   "Event.Attack.ComboWindow");
UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Attack_HitboxOn,      "Event.Attack.HitboxOn");
UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Attack_HitboxOff,     "Event.Attack.HitboxOff");
UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Attack_ChargeRelease, "Event.Attack.ChargeRelease");
UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Attack_ChannelStop,   "Event.Attack.ChannelStop");
UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Poise_LightFlinch,    "Event.Poise.LightFlinch");
UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Poise_HeavyFlinch,    "Event.Poise.HeavyFlinch");
UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Poise_StanceBreak,    "Event.Poise.StanceBreak");
UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Death,                "Event.Death");

// SetByCaller magnitudes.
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_Damage,       "Data.Damage");
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_Healing,      "Data.Healing");
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_PoiseDamage,  "Data.PoiseDamage");
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_ManaCost,     "Data.ManaCost");
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_StaminaCost,  "Data.StaminaCost");

// Equipment stat grants.
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_Equipment_Armor,                 "Data.Equipment.Armor");
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_Equipment_CritChance,            "Data.Equipment.CritChance");
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_Equipment_Resistance_Physical,   "Data.Equipment.Resistance.Physical");
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_Equipment_Resistance_Fire,       "Data.Equipment.Resistance.Fire");
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_Equipment_Resistance_Ice,        "Data.Equipment.Resistance.Ice");
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_Equipment_Resistance_Light,      "Data.Equipment.Resistance.Light");
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_Equipment_Resistance_Lightning,  "Data.Equipment.Resistance.Lightning");
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_Equipment_Resistance_Water,      "Data.Equipment.Resistance.Water");
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_Equipment_Resistance_Fall,       "Data.Equipment.Resistance.Fall");
