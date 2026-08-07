"""
Builds the sword UARPGWeaponAttackTree from the Godot sword_attack_tree.tres
structure.

Run headless:

    UnrealEditor-Cmd.exe <project>.uproject ^
        -ExecutePythonScript="Tools/build_sword_attack_tree.py" ^
        -unattended -nullrhi -nosplash

Depends on Tools/convert_attack_definitions.py having run first -- it looks the
attacks up by asset name.

ONE DELIBERATE DIFFERENCE FROM THE GODOT TREE. There, three leaf nodes
(low_spinning_slash, high_spinning_slash, overhead_slash) are single
sub-resources referenced from several parents AND from the parry follow-ups.
UE's Instanced properties cannot share an object that way, so each reference
becomes its own copy.

That is safe here because all three are leaves: they hold an attack and nothing
else, no follow-ups and no per-node overrides, so copies behave identically and
IsLeaf() agrees everywhere. The difference is in AUTHORING -- editing one copy no
longer edits the others. If a shared node ever needs per-node state, this has to
become a flat node array with plain references instead of nesting.
"""

import unreal


TREE_PACKAGE_PATH = "/Game/ARPG/Weapons"
TREE_ASSET_NAME = "DA_AttackTree_Sword"
ATTACKS_PATH = "/Game/ARPG/Attacks"


def attack(name):
    asset = unreal.EditorAssetLibrary.load_asset("{}/DA_Attack_{}".format(ATTACKS_PATH, name))
    if asset is None:
        raise RuntimeError("Missing attack asset: DA_Attack_{}".format(name))
    return asset


def make_tree_asset():
    package = "{}/{}".format(TREE_PACKAGE_PATH, TREE_ASSET_NAME)

    if unreal.EditorAssetLibrary.does_asset_exist(package):
        return unreal.EditorAssetLibrary.load_asset(package)

    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.ARPGWeaponAttackTree)
    return unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        TREE_ASSET_NAME, TREE_PACKAGE_PATH, unreal.ARPGWeaponAttackTree, factory
    )


def run():
    tree = make_tree_asset()
    if tree is None:
        unreal.log_error("Could not create the attack tree asset.")
        return

    def node(attack_name, light=None, heavy=None, special=None):
        """A combo node owned by the tree, so it serialises inside the asset."""
        n = unreal.new_object(unreal.ARPGComboAttackNode, outer=tree)
        n.set_editor_property("attack", attack(attack_name))
        if light is not None:
            n.set_editor_property("follow_light", light)
        if heavy is not None:
            n.set_editor_property("follow_heavy", heavy)
        if special is not None:
            n.set_editor_property("follow_special", special)
        return n

    # The three shared finishers. Built fresh per use -- see the module note.
    def low_spin():  return node("LowSpinningSlash")
    def high_spin(): return node("HighSpinningSlash")
    def overhead():  return node("OverheadSlash")

    # --- Light chain: one-handed -------------------------------------------
    # OH1 -light-> OH2 -light-> OH3 -{light,heavy,special}-> the three finishers
    #  |             `-heavy-> Kick
    #  `-heavy-> HiltSmack
    oh3 = node("OneHandComboAttack3",
               light=low_spin(), heavy=high_spin(), special=overhead())
    oh2 = node("OneHandComboAttack2", light=oh3, heavy=node("Kick"))
    oh1 = node("OneHandComboAttack1", light=oh2, heavy=node("HiltSmack"))

    # --- Heavy chain: cross slash into the two-handed chain -----------------
    # Cross -light-> TH1 -light-> TH2 -light-> TH3 -{...}-> the three finishers
    #   |-heavy-> CrossCombo1 -heavy-> CrossCombo2
    #   `-special-> DownwardSlash
    th3 = node("TwoHandComboAttack3",
               light=low_spin(), heavy=high_spin(), special=overhead())
    th2 = node("TwoHandComboAttack2", light=th3)
    th1 = node("TwoHandComboAttack1", light=th2)

    cross_combo_2 = node("CrossSlashComboAttack2")
    cross_combo_1 = node("CrossSlashComboAttack1", heavy=cross_combo_2)

    cross = node("CrossSlash",
                 light=th1, heavy=cross_combo_1, special=node("DownwardSlash"))

    # --- Special: jumping slash ---------------------------------------------
    jumping = node("JumpingSlash")

    tree.set_editor_property("root_light", oh1)
    tree.set_editor_property("root_heavy", cross)
    tree.set_editor_property("root_special", jumping)

    # Parry follow-ups reuse the three finishers, as in the Godot tree.
    tree.set_editor_property("parry_light", low_spin())
    tree.set_editor_property("parry_heavy", high_spin())
    tree.set_editor_property("parry_special", overhead())

    tree.set_editor_property("air_light", attack("JumpingSlash"))
    tree.set_editor_property("reset_timeout", 1.5)

    unreal.EditorAssetLibrary.save_loaded_asset(tree, only_if_is_dirty=False)

    # --- Report -------------------------------------------------------------
    def walk(n, depth, label, seen):
        if n is None:
            return 0
        atk = n.get_editor_property("attack")
        name = atk.get_editor_property("attack_id") if atk else "<none>"
        leaf = " [leaf]" if not any(
            n.get_editor_property(p) for p in ("follow_light", "follow_heavy", "follow_special")
        ) else ""
        unreal.log("    {}{}{}: {}{}".format("  " * depth, label, "" if depth else "", name, leaf))
        count = 1
        for prop, child_label in (("follow_light", "L"), ("follow_heavy", "H"), ("follow_special", "S")):
            count += walk(n.get_editor_property(prop), depth + 1, child_label + " ", seen)
        return count

    unreal.log("=" * 68)
    unreal.log("Sword attack tree: {}/{}".format(TREE_PACKAGE_PATH, TREE_ASSET_NAME))
    total = 0
    for root_prop, label in (("root_light", "ROOT LIGHT"),
                             ("root_heavy", "ROOT HEAVY"),
                             ("root_special", "ROOT SPECIAL")):
        unreal.log("  {}".format(label))
        total += walk(tree.get_editor_property(root_prop), 1, "", set())
    unreal.log("  PARRY FOLLOW-UPS")
    for prop, label in (("parry_light", "L"), ("parry_heavy", "H"), ("parry_special", "S")):
        total += walk(tree.get_editor_property(prop), 1, label + " ", set())
    unreal.log("  total nodes: {}".format(total))
    unreal.log("")
    unreal.log("  NOT SET: block and flinch definitions. Both are montage-shaped")
    unreal.log("  and are deferred until the retarget produces clips to point at.")
    unreal.log("=" * 68)


run()
