"""Creates the element palettes, elements, combination table and a loadout.

    "<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" <path>/arpg.uproject \
        -ExecutePythonScript="<path>/Tools/generate_element_assets.py" \
        -unattended -nopause -nosplash

THE NUMBERS ARE THE GODOT ORIGINAL'S, transcribed from project/magic/**.tres:
palettes, damage, poise, costs, usage rates, cloak durations and the whole
combination table including its lopsided consumption rates. They are balance
decisions that were already made and playtested, and re-inventing them here
would quietly fork the design.

RE-RUNNABLE. Existing assets are updated in place rather than replaced, so
anything hand-tuned in the editor is overwritten only for the fields this script
actually sets -- it does not touch VFX slots, icons or dodge data, which are
authoring work this cannot do.

WHAT IS DELIBERATELY LEFT EMPTY: every VFX slot. An element with a palette and
no authored effect falls back to the tinted placeholder, which is the state the
whole placeholder system exists to serve. Filling these in is what "finishing"
an element means.
"""

import unreal

ELEMENT_DIR = "/Game/ARPG/Magic/Elements"
PALETTE_DIR = "/Game/ARPG/Magic/Palettes"
MAGIC_DIR = "/Game/ARPG/Magic"

DAMAGE_TYPES = "/Game/ARPG/DamageTypes"

STATUS = "/Script/ARPGCombat.ARPGStatusEffect_{}"

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def log(message):
    unreal.log("[ARPG elements] {}".format(message))


def colour(r, g, b, a=1.0):
    return unreal.LinearColor(r, g, b, a)


def tag(name):
    """FGameplayTag has no Python constructor; its text form is the way in."""
    result = unreal.GameplayTag()
    result.import_text(name)

    # A tag that is not in the registry imports as a well-formed struct that
    # matches nothing, so nothing would ever combine and there would be no error
    # anywhere to say why. Checked here, where the name is still in hand.
    if not unreal.GameplayTagLibrary.is_gameplay_tag_valid(result):
        log("'{}' is not a registered gameplay tag".format(name))

    return result


REACTION_MODES = {
    0: unreal.ARPGReactionMode.AUTO,
    1: unreal.ARPGReactionMode.CONDUCT,
    2: unreal.ARPGReactionMode.SOLIDIFY,
}


# name -> palette, then element. Palette colours are the .tres values verbatim.
#
# Air and earth take PHYSICAL damage on purpose: neither is a damage type of its
# own in this game, and inventing one would mean armour and resistance had
# nothing authored to say about them.
ELEMENTS = [
    {
        "id": "Fire", "tag": "Element.Fire", "display": "Fire",
        "core": colour(1.0, 0.95, 0.75), "glow": colour(1.0, 0.45, 0.08, 0.95),
        "edge": colour(0.35, 0.06, 0.02, 0.0), "emission": 4.0,
        "damage_type": "DA_Damage_Fire",
        "base_damage": 18.0, "poise": 5.0, "cost": 5.0, "usage": 1.0,
        "status": "Burning", "status_duration": 6.0,
        "cloak_duration": 5.0, "cloak_bonus": 5.0, "cloak_self": "Burning",
    },
    {
        "id": "Water", "tag": "Element.Water", "display": "Water",
        "core": colour(0.9, 0.98, 1.0), "glow": colour(0.2, 0.55, 0.95, 0.9),
        "edge": colour(0.05, 0.18, 0.45, 0.0), "emission": 2.0,
        "damage_type": "DA_Damage_Water",
        "base_damage": 12.0, "poise": 0.0, "cost": 5.0, "usage": 1.0,
        "status": "Wet", "status_duration": 10.0,
    },
    {
        "id": "Lightning", "tag": "Element.Lightning", "display": "Lightning",
        "core": colour(0.95, 0.98, 1.0), "glow": colour(0.45, 0.68, 1.0, 0.95),
        "edge": colour(0.2, 0.35, 0.9, 0.0), "emission": 6.0,
        "damage_type": "DA_Damage_Lightning",
        "base_damage": 16.0, "poise": 4.0, "cost": 5.0, "usage": 1.2,
        "status": "Shocked", "status_duration": 4.0,
        # The one element that travels through a medium rather than reacting
        # with it -- see the conduct row in the table below.
        "conduction": "Shocked",
    },
    {
        "id": "Air", "tag": "Element.Air", "display": "Air",
        "core": colour(1.0, 1.0, 1.0, 0.85), "glow": colour(0.8, 0.92, 0.9, 0.55),
        "edge": colour(0.6, 0.75, 0.75, 0.0), "emission": 1.2,
        "damage_type": "DA_Damage_Physical",
        # Low damage, high poise: air shoves rather than wounds.
        "base_damage": 8.0, "poise": 8.0, "cost": 3.0, "usage": 0.8,
    },
    {
        "id": "Earth", "tag": "Element.Earth", "display": "Earth",
        "core": colour(0.8, 0.7, 0.55), "glow": colour(0.45, 0.34, 0.22, 0.95),
        "edge": colour(0.2, 0.15, 0.1, 0.0), "emission": 0.8,
        "damage_type": "DA_Damage_Physical",
        "base_damage": 10.0, "poise": 0.0, "cost": 5.0, "usage": 1.0,
    },
    {
        "id": "Ice", "tag": "Element.Ice", "display": "Ice",
        "core": colour(0.95, 1.0, 1.0), "glow": colour(0.6, 0.85, 1.0, 0.92),
        "edge": colour(0.3, 0.55, 0.85, 0.0), "emission": 2.5,
        "damage_type": "DA_Damage_Ice",
        # Cost 0 and complexity 2: a product is paid for by the elements that
        # made it, not readied directly.
        "base_damage": 20.0, "poise": 6.0, "cost": 0.0, "usage": 1.3,
        "complexity": 2,
    },
    {
        "id": "Steam", "tag": "Element.Steam", "display": "Steam",
        "core": colour(1.0, 1.0, 1.0, 0.9), "glow": colour(0.88, 0.92, 0.95, 0.55),
        "edge": colour(0.55, 0.7, 0.8, 0.0), "emission": 1.0,
        # FIRE damage, as in the original: steam scalds.
        "damage_type": "DA_Damage_Fire",
        "base_damage": 25.0, "poise": 0.0, "cost": 0.0, "usage": 1.5,
        "complexity": 2,
    },
]

# Scope bits: Hand 1, Collision 2, Field 4, Surface 8. Mode: Auto 0, Conduct 1,
# Solidify 2. Both transcribed from the original table.
COMBINATIONS = [
    # Fire + water in hand or anywhere: steam. Water is consumed six times
    # faster than fire, which is what makes quenching directional.
    {"name": "Steam", "elements": ["Element.Fire", "Element.Water"],
     "result": "Steam", "scope": 0xF, "rates": {"Element.Fire": 1.6, "Element.Water": 0.25}},

    # Fire + air -> FIRE. The result being one of the reactants is what makes
    # this amplification rather than transmutation: wind fans a fire.
    {"name": "FannedFire", "elements": ["Element.Fire", "Element.Air"],
     "result": "Fire", "scope": 2 | 4, "rates": {"Element.Fire": 0.0, "Element.Air": 1.0}},

    # Only in HAND. Air and water meeting in the world is weather, not ice.
    {"name": "IceRecipe", "elements": ["Element.Air", "Element.Water"],
     "result": "Ice", "scope": 1},

    {"name": "Freeze", "elements": ["Element.Ice", "Element.Water"],
     "result": "Ice", "scope": 2 | 4,
     "rates": {"Element.Ice": 0.0, "Element.Water": 1.0}, "amplification": 0.7},

    # SURFACE scope with the solidify mode: the same pair that amplifies in a
    # collision instead freezes a standable slab on a body of water.
    {"name": "FreezeSurface", "elements": ["Element.Ice", "Element.Water"],
     "result": "Ice", "scope": 8, "mode": 2,
     "rates": {"Element.Ice": 1.0, "Element.Water": 0.0}},

    {"name": "Melt", "elements": ["Element.Fire", "Element.Ice"],
     "result": "Water", "scope": 2,
     "rates": {"Element.Fire": 0.6, "Element.Ice": 1.4}},

    # CONDUCT: lightning travels through water rather than reacting with it, so
    # the water is a carrier and is not consumed.
    {"name": "ConductWater", "elements": ["Element.Lightning", "Element.Water"],
     "result": "Lightning", "scope": 2, "mode": 1},
]

# Page 0 is the four a new character starts with; lightning sits on page 1 so
# the D-pad paging has something to page to.
LOADOUT = [
    (0, "North", "Fire"),
    (0, "West", "Water"),
    (0, "South", "Air"),
    (0, "East", "Earth"),
    (1, "North", "Lightning"),
]

SLOT_INDEX = {"North": 0, "West": 1, "South": 2, "East": 3}
SLOT_COUNT = 4


def ensure(package_path, name, asset_class, factory=None):
    path = "{}/{}".format(package_path, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        return unreal.EditorAssetLibrary.load_asset(path)

    unreal.EditorAssetLibrary.make_directory(package_path)
    asset = asset_tools.create_asset(
        asset_name=name, package_path=package_path,
        asset_class=asset_class, factory=factory)
    log("created {}".format(path))
    return asset


def load_status_class(name):
    if not name:
        return None
    loaded = unreal.load_class(None, STATUS.format(name))
    if not loaded:
        log("no status class for '{}'".format(name))
    return loaded


def main():
    elements = {}

    for spec in ELEMENTS:
        palette = ensure(PALETTE_DIR, "DA_Palette_{}".format(spec["id"]),
                         unreal.ARPGElementPalette)
        palette.set_editor_property("core", spec["core"])
        palette.set_editor_property("glow", spec["glow"])
        palette.set_editor_property("edge", spec["edge"])
        palette.set_editor_property("emission_strength", spec["emission"])
        unreal.EditorAssetLibrary.save_loaded_asset(palette)

        element = ensure(ELEMENT_DIR, "DA_Element_{}".format(spec["id"]),
                         unreal.ARPGMagicElement)
        element.set_editor_property("element_tag", tag(spec["tag"]))
        element.set_editor_property("display_name", unreal.Text(spec["display"]))
        element.set_editor_property("palette", palette)
        element.set_editor_property("base_damage", spec["base_damage"])
        element.set_editor_property("base_poise_damage", spec["poise"])
        element.set_editor_property("activation_cost", spec["cost"])
        element.set_editor_property("usage_rate", spec["usage"])
        element.set_editor_property("complexity", spec.get("complexity", 1))

        damage_type = unreal.EditorAssetLibrary.load_asset(
            "{}/{}".format(DAMAGE_TYPES, spec["damage_type"]))
        if damage_type:
            element.set_editor_property("damage_type", damage_type)
        else:
            log("missing damage type {} for {}".format(spec["damage_type"], spec["id"]))

        status = load_status_class(spec.get("status"))
        if status:
            element.set_editor_property("on_hit_effect", status)
            element.set_editor_property("status_duration", spec.get("status_duration", 0.0))

        conduction = load_status_class(spec.get("conduction"))
        if conduction:
            element.set_editor_property("conduction_effect", conduction)

        cloak_self = load_status_class(spec.get("cloak_self"))
        if cloak_self:
            element.set_editor_property("cloak_self_effect", cloak_self)
        element.set_editor_property("cloak_base_duration", spec.get("cloak_duration", 0.0))
        element.set_editor_property("cloak_charge_bonus", spec.get("cloak_bonus", 0.0))

        unreal.EditorAssetLibrary.save_loaded_asset(element)
        elements[spec["id"]] = element

    # --- Combination table --------------------------------------------------

    table = ensure(MAGIC_DIR, "DA_MagicCombinations", unreal.ARPGMagicCombinationTable)

    entries = []
    for combo in COMBINATIONS:
        # Instanced sub-objects of the table, which is how the table declares
        # them: EditInlineNew, so they have no assets of their own.
        entry = unreal.new_object(unreal.ARPGMagicCombinationEntry, outer=table)

        required = unreal.GameplayTagLibrary.make_gameplay_tag_container_from_array(
            [tag(element_tag) for element_tag in combo["elements"]])

        entry.set_editor_property("required_elements", required)
        entry.set_editor_property("result", elements[combo["result"]])
        entry.set_editor_property("scope", combo.get("scope", 0xF))
        entry.set_editor_property("mode", REACTION_MODES[combo.get("mode", 0)])
        entry.set_editor_property("priority", combo.get("priority", 0))
        entry.set_editor_property("amplification_efficiency", combo.get("amplification", 0.6))

        rates = combo.get("rates")
        if rates:
            entry.set_editor_property("consumption_rates",
                                      {tag(k): v for k, v in rates.items()})

        entries.append(entry)

    table.set_editor_property("entries", entries)
    unreal.EditorAssetLibrary.save_loaded_asset(table)
    log("table has {} entries".format(len(entries)))

    # --- Loadout ------------------------------------------------------------

    loadout = ensure(MAGIC_DIR, "DA_MagicLoadout_Starter", unreal.ARPGMagicLoadout)

    page_count = max(page for page, _, _ in LOADOUT) + 1
    loadout.set_editor_property("page_count", page_count)
    loadout.set_editor_property("max_elements", 2)

    # Written as the flat array the asset actually stores: page * 4 + slot.
    slots = [None] * (page_count * SLOT_COUNT)
    for page, slot_name, element_id in LOADOUT:
        slots[page * SLOT_COUNT + SLOT_INDEX[slot_name]] = elements[element_id]

    loadout.set_editor_property("elements", slots)
    unreal.EditorAssetLibrary.save_loaded_asset(loadout)
    log("loadout has {} pages".format(page_count))


main()
