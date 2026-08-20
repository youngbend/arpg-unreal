"""Creates a test NPC: a sword weapon definition, an archetype, and a Manny Blueprint.

    "<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" <path>/arpg.uproject \
        -ExecutePythonScript="<path>/Tools/generate_test_npc.py" \
        -unattended -nopause -nosplash

WHY A WEAPON DEFINITION IS PART OF THIS. The sword's ATTACK TREE has existed
since phase 6, but no UARPGWeaponDefinition ever wrapped it -- the player
character reaches the tree through a hard-coded soft path instead. An NPC cannot
do that: UARPGNPCComponent equips a weapon DEFINITION, and the AI's attack-range
condition reads Reach off it. So the definition is not incidental packaging here,
it is the thing that makes the archetype's reach and moveset real.

RE-RUNNABLE. Existing assets are updated in place rather than replaced, so
anything hand-tuned in the editor survives except the fields this script sets.

WHAT IT DOES NOT DO: the StateTree. See generate_npc_statetree.py, and the
honest warning in it. This script leaves the archetype's StateTreeRef alone if it
is already set, and reports that the NPC will stand still until one exists.
"""

import unreal

NPC_DIR = "/Game/ARPG/NPCs"
SWORD_DIR = "/Game/ARPG/Weapons/sword"

ATTACK_TREE = "/Game/ARPG/Weapons/sword/DA_AttackTree_Sword"
PHYSICAL_DAMAGE = "/Game/ARPG/DamageTypes/DA_Damage_Physical"

# The template's own Manny. SKM_Manny_Simple rather than the full SKM_Manny
# because it is what this project actually has -- see Content/Characters.
MANNY_MESH = "/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple"
UNARMED_ABP = "/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed"

STATE_TREE = "/Game/ARPG/NPCs/ST_NPC_Melee"

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def log(message):
    unreal.log("[ARPG test NPC] {}".format(message))


def save(asset, path):
    """Saves, and treats a refusal as an error rather than a silent no-op.

    THE RETURN VALUE MATTERS. A package whose file is held open by another
    process -- an editor with the asset loaded is enough -- fails to save with
    only a warning, while the object in memory still holds everything you just
    wrote. Verifying by reading the loaded object back therefore reports success
    against a file that never changed.
    """
    if unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
        return True

    log("SAVE REFUSED for {} -- is the editor open with this asset loaded?".format(path))
    return False


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


def load_or_warn(path, what):
    if not unreal.EditorAssetLibrary.does_asset_exist(path):
        log("MISSING {}: {} -- leaving that field empty".format(what, path))
        return None
    return unreal.EditorAssetLibrary.load_asset(path)


def faction_tag(name):
    """FGameplayTag has no Python constructor; its text form is the way in.

    The same idiom as generate_element_assets.py, and for the same reason:
    GameplayTagLibrary exposes no request_gameplay_tag. Its Blueprint-facing
    surface is containers and queries, and building one tag from a name is not
    part of it -- so the struct is default-constructed and imported into.
    """
    result = unreal.GameplayTag()
    result.import_text(name)

    # A tag missing from the registry imports as a well-formed struct that
    # matches NOTHING, which would leave this NPC hostile to nobody and hostile
    # in no way that any faction check could see. Reported here, where the name
    # is still in hand to put in the message.
    if not unreal.GameplayTagLibrary.is_gameplay_tag_valid(result):
        log("'{}' is not a registered gameplay tag".format(name))

    return result


def make_weapon():
    """The sword, as a definition rather than a bare attack tree."""
    weapon = ensure(SWORD_DIR, "DA_Weapon_Sword", unreal.ARPGWeaponDefinition)

    weapon.set_editor_property("weapon_id", unreal.Name("sword"))
    weapon.set_editor_property("display_name", unreal.Text("Sword"))

    # 150 is the class default and it is the RIGHT number to be explicit about:
    # the AI's attack-range condition adds its tolerance to this, so it decides
    # how close an NPC walks before it swings.
    weapon.set_editor_property("base_damage", 25.0)
    weapon.set_editor_property("reach", 180.0)

    tree = load_or_warn(ATTACK_TREE, "attack tree")
    if tree:
        weapon.set_editor_property("attack_tree", tree)

    damage_type = load_or_warn(PHYSICAL_DAMAGE, "damage type")
    if damage_type:
        weapon.set_editor_property("base_damage_type", damage_type)

    save(weapon, "{}/DA_Weapon_Sword".format(SWORD_DIR))
    return weapon


def make_definition(weapon):
    """The archetype. Everything a goblin IS, as data."""
    definition = ensure(NPC_DIR, "DA_NPC_TestDummy", unreal.ARPGNPCDefinition)

    definition.set_editor_property("npc_id", unreal.Name("TestDummy"))
    definition.set_editor_property("display_name", unreal.Text("Test Dummy"))
    definition.set_editor_property("faction_tag", faction_tag("Faction.Enemy"))

    if weapon:
        definition.set_editor_property("weapon", weapon)

    # THE LOADED OBJECT, not a SoftObjectPath. Mesh is a TSoftObjectPtr, and the
    # bridge builds the soft pointer from a UObject -- handed the FSoftObjectPath
    # that seems like the closer match it refuses the conversion outright.
    mesh = load_or_warn(MANNY_MESH, "Manny mesh")
    if mesh:
        definition.set_editor_property("mesh", mesh)

    definition.set_editor_property("max_health", 200.0)
    definition.set_editor_property("max_poise", 60.0)

    definition.set_editor_property("move_speed", 380.0)

    # Slower than the player's 500, deliberately. The turn is the tell the parry
    # window is read off, and a test NPC that snaps to face you gives the player
    # nothing to react to.
    definition.set_editor_property("turn_rate_degrees", 260.0)

    definition.set_editor_property("detection_range", 1600.0)
    definition.set_editor_property("leash_range", 3000.0)

    # Investigate rather than Aggro, so the test NPC exercises the more
    # interesting half of the damage channel: shot from out of sight it walks
    # over to look instead of turning and knowing exactly where you are.
    definition.set_editor_property(
        "damage_reaction", unreal.ARPGDamageReaction.INVESTIGATE)

    if unreal.EditorAssetLibrary.does_asset_exist(STATE_TREE):
        tree = unreal.EditorAssetLibrary.load_asset(STATE_TREE)
        reference = definition.get_editor_property("state_tree_ref")
        reference.set_editor_property("state_tree", tree)
        definition.set_editor_property("state_tree_ref", reference)
        log("archetype points at {}".format(STATE_TREE))
    else:
        log("NO STATE TREE at {} -- this NPC will perceive, take damage and "
            "stand there. Run generate_npc_statetree.py, or author one and set "
            "StateTreeRef by hand.".format(STATE_TREE))

    save(definition, "{}/DA_NPC_TestDummy".format(NPC_DIR))
    return definition


def make_blueprint(definition):
    """A Blueprint over AARPGNPCCharacter carrying Manny's mesh.

    A BLUEPRINT AT ALL, rather than placing the C++ class directly, because the
    mesh and anim Blueprint are CONTENT: naming them in C++ is the hard-coded
    path that has already broken this project once (see AarpgCharacter's
    DefaultAttackTree comment). The archetype names its mesh too, for spawners
    that build an NPC from data alone -- this is the placed-in-a-level path.
    """
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", unreal.ARPGNPCCharacter)

    blueprint = ensure(NPC_DIR, "BP_NPC_Manny", None, factory)
    if not blueprint:
        log("could not create BP_NPC_Manny")
        return None

    cdo = unreal.get_default_object(blueprint.generated_class())

    if definition:
        npc_component = cdo.get_editor_property("npc_component")
        if npc_component:
            npc_component.set_editor_property("definition", definition)
            log("Blueprint's NPC component points at the archetype")

    # The mesh and its offset. Manny's origin is at the feet and faces +Y, so the
    # template's own -90 yaw and -96 Z are what put it inside the capsule facing
    # forward; anything else and the AI's facing checks measure a sideways model.
    mesh_component = cdo.get_editor_property("mesh")
    if mesh_component:
        skeletal = load_or_warn(MANNY_MESH, "Manny mesh")
        if skeletal:
            mesh_component.set_editor_property("skeletal_mesh_asset", skeletal)

        anim_bp = load_or_warn(UNARMED_ABP, "unarmed anim Blueprint")
        if anim_bp:
            mesh_component.set_editor_property("anim_class", anim_bp.generated_class())

        mesh_component.set_editor_property(
            "relative_location", unreal.Vector(0.0, 0.0, -96.0))
        mesh_component.set_editor_property(
            "relative_rotation", unreal.Rotator(0.0, 0.0, -90.0))
    else:
        log("could not reach the Blueprint's mesh component -- assign "
            "SKM_Manny_Simple and ABP_Unarmed by hand in the Blueprint editor")

    unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
    save(blueprint, "{}/BP_NPC_Manny".format(NPC_DIR))
    return blueprint


def main():
    unreal.EditorAssetLibrary.make_directory(NPC_DIR)

    weapon = make_weapon()
    definition = make_definition(weapon)
    make_blueprint(definition)

    log("done. Drop BP_NPC_Manny into a level with a NavMeshBoundsVolume -- "
        "the retreat task needs a navmesh, and without one it reports that it "
        "had nowhere to go.")


main()
