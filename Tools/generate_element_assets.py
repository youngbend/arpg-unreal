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
ATTACK_DIR = "/Game/ARPG/Attacks"

DAMAGE_TYPES = "/Game/ARPG/DamageTypes"

STATUS = "/Script/ARPGCombat.ARPGStatusEffect_{}"

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def log(message):
    unreal.log("[ARPG elements] {}".format(message))


def save(asset, path):
    """Saves, and treats a refusal as an error rather than a silent no-op.

    THE RETURN VALUE MATTERS. A package whose file is held open by another
    process -- an editor with the asset loaded is enough -- fails to save with
    only a warning, and the object in memory still holds everything you just
    wrote. Verifying by reading the loaded object back therefore reports success
    against a file that never changed, which is exactly how a broken asset
    survived several "verified" runs of this script.
    """
    if unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
        return True

    log("SAVE REFUSED for {} -- is the editor open with this asset loaded?".format(path))
    return False


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
    {
        # NOT IN THE GODOT ORIGINAL. The first element authored for Unreal, and
        # the first SPECIFIC-ATTACK imbue: readying it and swinging performs one
        # of magnetism's own moves instead of coating the weapon, and ends the
        # chain. Everything above is a continuation imbue.
        #
        # It earns the type by not being expressible as one. Every other element
        # answers "what is this hit made of", and could therefore be a coating.
        # Magnetism takes the weapon out of your hands and flies it -- which is
        # not a property of the hit at all, it is a different attack. No damage
        # type can say that, so it gets three moves instead.
        "id": "Magnetism", "tag": "Element.Magnetism", "display": "Magnetism",
        "core": colour(0.72, 0.74, 0.82), "glow": colour(0.35, 0.42, 0.72, 0.95),
        "edge": colour(0.12, 0.14, 0.3, 0.0), "emission": 3.0,
        # LIGHTNING damage: it is the electromagnetic half doing the hurting, and
        # reusing the type keeps this off the damage execution's capture list.
        "damage_type": "DA_Damage_Lightning",
        "base_damage": 18.0, "poise": 5.0, "cost": 0.0, "usage": 1.3,
        "complexity": 2,
        "status": "Shocked", "status_duration": 3.0,
        # A FIELD, NOT AN EDGE. The coating measures itself against the swing's
        # hitbox AFTER that attack's own hitbox_radius has been applied, so these
        # compound: the attack says how far out the weapon is flying, and this
        # says how far the field extends past it. 1.6 on the floating slash's 2.2
        # puts the lightning at 3.5x a held blade's reach.
        "imbue_reach": 1.6,
        "imbue_type": unreal.ARPGImbueType.SPECIFIC_ATTACK,
        "imbue_attacks": {
            "light": "MagnetismFloatingSlash",
            "heavy": "MagnetismSpin",
            "special": "MagnetismToss",
        },
    },
]

# The three moves magnetism performs instead of a swing. All three are the same
# idea -- the weapon leaves the hand and is flown by the field -- read three ways.
#
# MONTAGES ARE LEFT UNSET, exactly as the VFX slots are: animation is authoring
# work this script cannot do, and the melee ability already logs and releases the
# combo cleanly for an attack with no montage.
#
# THE ANIMATION CARRIES THE DISTANCE. The hitbox is attached to the weapon socket
# and sweeps between frames, so a montage that sends the blade out on its own
# extends the attack's reach for free and cannot tunnel past anything on the way.
# hitbox_radius is only the looseness on top of that -- a blade flown at range
# connects broadly, not on its edge.
#
# chain_bonus is the payoff for where the move was thrown from. Every one of
# these ends the chain, so each is worth more the more chain it spends: at depth
# 3 the light swings for 1.75x and the toss for 2.05x. Thrown from neutral they
# are worth exactly their base, which is the trade the whole type rests on.
MAGNETISM_ATTACKS = [
    {
        # Light: the sword lets go and swings on its own, further out than an arm
        # reaches. The cheapest of the three and the one that still feels like a
        # sword attack -- it is the swing, just not held.
        "id": "MagnetismFloatingSlash", "display": "Floating Slash",
        "motion_value": 1.0, "hitbox_radius": 2.2,
        "poise": 3.0, "poise_scales": False, "knockback": 250.0,
        "stamina": 10.0, "speed_factor": 0.5, "lockout": 0.2, "hit_stop": 0.06,
        "chain_bonus": 0.25,
    },
    {
        # Heavy: the weapon spins in front of the player. MULTIHIT is the whole
        # move, and it is the hitbox's re-hit interval rather than anything new --
        # the same mechanism a drill attack uses. Low motion value per tick,
        # because it lands many of them.
        #
        # KNOCKBACK IS ZERO ON PURPOSE. Any shove per tick would push the target
        # straight out of the spin and turn a blender into one hit.
        "id": "MagnetismSpin", "display": "Spinning Guard",
        "motion_value": 0.45, "hitbox_radius": 1.6, "tick_interval": 0.12,
        "poise": 2.5, "poise_scales": True, "knockback": 0.0,
        "stamina": 18.0, "speed_factor": 0.25, "lockout": 0.45, "hit_stop": 0.04,
        "hyperarmor": True, "chain_bonus": 0.2,
    },
    {
        # Special: thrown like a spear and yanked back into the hand. TWO
        # WINDOWS, which the attack definition already speaks -- motion_value is
        # the throw and motion_value2 the return, so a target in the lane is hit
        # going out and again coming back.
        "id": "MagnetismToss", "display": "Rail Toss",
        "motion_value": 1.6, "motion_value_2": 1.2, "hitbox_radius": 3.0,
        "poise": 6.0, "poise_scales": True, "knockback": 400.0,
        "stamina": 22.0, "speed_factor": 0.1, "lockout": 0.8, "hit_stop": 0.15,
        "unblockable": True, "chain_bonus": 0.35,
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

    # Only in HAND, on the same reasoning as the ice recipe: earth and lightning
    # meeting out in the world is a scorched rock, not a magnetic field. Holding
    # both and MEANING it is what makes the difference.
    {"name": "MagnetismRecipe", "elements": ["Element.Earth", "Element.Lightning"],
     "result": "Magnetism", "scope": 1},
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


def make_imbue_attacks():
    """The attack definitions a specific-attack element performs.

    Created here rather than in build_sword_attack_tree.py because they belong
    to the ELEMENT, not to any weapon's moveset -- they are never reached
    through a combo tree, so nothing would ever link them up there.
    """
    made = {}

    for spec in MAGNETISM_ATTACKS:
        name = "DA_Attack_{}".format(spec["id"])
        asset = ensure(ATTACK_DIR, name, unreal.ARPGAttackDefinition)

        asset.set_editor_property("attack_id", spec["id"])
        asset.set_editor_property("display_name", unreal.Text(spec["display"]))
        asset.set_editor_property("motion_value", spec["motion_value"])
        asset.set_editor_property("motion_value2", spec.get("motion_value_2", 1.0))
        asset.set_editor_property("hitbox_radius_scale", spec.get("hitbox_radius", 1.0))
        asset.set_editor_property("hitbox_tick_interval", spec.get("tick_interval", 0.0))
        asset.set_editor_property("poise_damage", spec["poise"])

        # Scaled where the move's payoff should grow with the chain it spent,
        # absolute where the stagger is a fixed utility the player is relying on.
        asset.set_editor_property("poise_scales_with_motion", spec.get("poise_scales", True))

        asset.set_editor_property("knockback_power", spec["knockback"])

        # What makes ending the chain a decision rather than a tax.
        asset.set_editor_property("chain_depth_bonus", spec.get("chain_bonus", 0.0))
        asset.set_editor_property("max_chain_depth", spec.get("max_chain_depth", 3))
        asset.set_editor_property("stamina_cost", spec["stamina"])
        asset.set_editor_property("movement_speed_factor", spec["speed_factor"])
        asset.set_editor_property("finisher_lockout", spec["lockout"])
        asset.set_editor_property("hit_stop_duration", spec["hit_stop"])
        asset.set_editor_property("hyperarmor", spec.get("hyperarmor", False))
        asset.set_editor_property("unblockable", spec.get("unblockable", False))

        # These ARE the element's attacks, so the coating applies to them: the
        # weapon hitbox swings the steel, and magnetism's own wider hitbox
        # carries the lightning and the displacement out to field range.
        asset.set_editor_property("can_be_elemental", True)

        save(asset, "{}/{}".format(ATTACK_DIR, name))
        made[spec["id"]] = asset

    return made


def main():
    elements = {}
    imbue_attacks = make_imbue_attacks()

    for spec in ELEMENTS:
        palette = ensure(PALETTE_DIR, "DA_Palette_{}".format(spec["id"]),
                         unreal.ARPGElementPalette)
        palette.set_editor_property("core", spec["core"])
        palette.set_editor_property("glow", spec["glow"])
        palette.set_editor_property("edge", spec["edge"])
        palette.set_editor_property("emission_strength", spec["emission"])
        save(palette, "{}/DA_Palette_{}".format(PALETTE_DIR, spec["id"]))

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

        # --- Imbue --------------------------------------------------------
        # Left at the defaults for every continuation element, which is all of
        # them but one: reach 1 keeps the coating on the blade's edge, and the
        # default type coats rather than replaces.
        element.set_editor_property("imbue_reach_scale", spec.get("imbue_reach", 1.0))
        element.set_editor_property(
            "imbue_type", spec.get("imbue_type", unreal.ARPGImbueType.CONTINUATION))

        for button, attack_id in spec.get("imbue_attacks", {}).items():
            attack = imbue_attacks.get(attack_id)
            if attack:
                element.set_editor_property("imbue_attack_{}".format(button), attack)
            else:
                log("missing imbue attack {} for {}".format(attack_id, spec["id"]))

        save(element, "{}/DA_Element_{}".format(ELEMENT_DIR, spec["id"]))
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
    save(table, "{}/DA_MagicCombinations".format(MAGIC_DIR))
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
    save(loadout, "{}/DA_MagicLoadout_Starter".format(MAGIC_DIR))
    log("loadout has {} pages".format(page_count))


main()
