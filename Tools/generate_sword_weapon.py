"""Creates the sword UARPGWeaponDefinition that owns the sword attack tree.

Run headless:

    "<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" <path>/arpg.uproject \
        -ExecutePythonScript="<path>/Tools/generate_sword_weapon.py" \
        -unattended -nopause -nosplash

WHY THIS EXISTS. The sword's moveset was reaching the player without any weapon
in hand: AarpgCharacter seeded the combo component's FALLBACK tree -- the
bare-handed moveset -- with DA_AttackTree_Sword, so every sword combo was
available unarmed and unequipping took nothing away. The character now seeds a
default WEAPON instead, and the tree arrives only while that weapon is equipped.
That needs a UARPGWeaponDefinition to exist, and the project shipped attacks, a
tree and montages without ever authoring one.

RE-RUNNABLE. An existing asset is updated in place, and only for the fields set
here -- the mesh, icon and sheath socket are authoring work this cannot do.

THE DAMAGE NUMBER IS DELIBERATELY CONSERVATIVE. 10 is what UARPGHitboxComponent
ships with as WeaponBaseDamage, which is what every sword swing was actually
dealing while the moveset was coming through the unarmed slot -- nothing ever
wrote a weapon's damage onto the hitbox because nothing was ever equipped. So
this keeps hits landing for exactly what they landed for before, and is the
first thing to tune once the weapon is real.
"""

import unreal


WEAPON_DIR = "/Game/ARPG/Weapons/sword"
WEAPON_NAME = "DA_Weapon_Sword"

# Where build_sword_attack_tree.py writes, and where the tree actually lives
# today -- the sword content was moved under Weapons/sword/ after that script was
# written. Both are checked so a fresh run of either order still finds it.
TREE_PATHS = [
    "/Game/ARPG/Weapons/sword/DA_AttackTree_Sword",
    "/Game/ARPG/Weapons/DA_AttackTree_Sword",
]

PHYSICAL_DAMAGE = "/Game/ARPG/DamageTypes/DA_Damage_Physical"

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def log(message):
    unreal.log("[ARPG sword] {}".format(message))


def find_attack_tree():
    for path in TREE_PATHS:
        if unreal.EditorAssetLibrary.does_asset_exist(path):
            return unreal.EditorAssetLibrary.load_asset(path), path
    return None, None


def ensure_weapon():
    path = "{}/{}".format(WEAPON_DIR, WEAPON_NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        return unreal.EditorAssetLibrary.load_asset(path), path

    unreal.EditorAssetLibrary.make_directory(WEAPON_DIR)

    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.ARPGWeaponDefinition)
    asset = asset_tools.create_asset(
        asset_name=WEAPON_NAME, package_path=WEAPON_DIR,
        asset_class=unreal.ARPGWeaponDefinition, factory=factory)
    log("created {}".format(path))
    return asset, path


def run():
    weapon, path = ensure_weapon()
    if weapon is None:
        unreal.log_error("Could not create {}/{}".format(WEAPON_DIR, WEAPON_NAME))
        return

    tree, tree_path = find_attack_tree()
    if tree is None:
        # Not fatal: the weapon is still worth having, and the combo component
        # warns clearly about a weapon with no moveset. But say so loudly,
        # because an equipped sword that does nothing is the confusing case.
        unreal.log_error(
            "No sword attack tree found at any of {} -- run "
            "Tools/build_sword_attack_tree.py first, or the sword will equip "
            "with no moveset.".format(", ".join(TREE_PATHS)))
    else:
        log("attack tree: {}".format(tree_path))

    weapon.set_editor_property("weapon_id", "sword")
    weapon.set_editor_property("display_name", unreal.Text("Sword"))
    weapon.set_editor_property("weapon_type", unreal.ARPGWeaponType.SWORD)
    weapon.set_editor_property("rarity", unreal.ARPGRarity.COMMON)

    # See the module note: the hitbox's shipped WeaponBaseDamage, so equipping
    # this changes what a swing MEANS without changing what it hits for.
    weapon.set_editor_property("base_damage", 10.0)

    # Doubles as the AI's attack range, so it is not a cosmetic number.
    weapon.set_editor_property("reach", 150.0)

    damage_type = unreal.EditorAssetLibrary.load_asset(PHYSICAL_DAMAGE)
    if damage_type:
        weapon.set_editor_property("base_damage_type", damage_type)
    else:
        log("missing {}; swings will be untyped".format(PHYSICAL_DAMAGE))

    if tree is not None:
        weapon.set_editor_property("attack_tree", tree)

    if not unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
        unreal.log_error(
            "SAVE REFUSED for {} -- is the editor open with this asset "
            "loaded?".format(path))
        return

    log("=" * 60)
    log("{}: {} damage, {}cm reach, tree {}".format(
        path, 10.0, 150.0, tree_path or "<NONE>"))
    log("")
    log("  NOT SET: weapon mesh, icon and sheath point. All three are")
    log("  presentation authoring; the weapon is mechanically complete without")
    log("  them, and rides an invisible socket until they are filled in.")
    log("=" * 60)


run()
