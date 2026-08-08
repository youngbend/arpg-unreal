"""
Builds the IK Rigs and retargeter, then retargets the paladin attacks onto the
UE5 mannequin.

    UnrealEditor-Cmd.exe <project>.uproject ^
        -ExecutePythonScript="Tools/retarget_to_mannequin.py" ^
        -unattended -nullrhi -nosplash

Chain names must be IDENTICAL on both rigs -- that name match IS the mapping.
A typo does not error; the limb simply never moves.

Bone names are validated against each skeleton before a chain is created, so a
wrong guess is reported rather than silently producing a dead chain.
"""

import unreal


PALADIN_MESH = "/Game/ARPG/Animations/Paladin/Rig/paladin/SkeletalMeshes/Paladin_character"
MANNY_MESH = "/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple"

RIG_DIR = "/Game/ARPG/Animations/Rigs"
ANIM_SEARCH_DIR = "/Game/ARPG/Animations/Paladin/Attacks"
RETARGET_DEST = "/Game/ARPG/Animations/Mannequin"

IK_PALADIN = RIG_DIR + "/IK_Paladin"
IK_MANNY = RIG_DIR + "/IK_Manny"
RETARGETER = RIG_DIR + "/RTG_Paladin_To_Manny"

# chain name -> (start, end). Identical keys on both sides is the whole mapping.
PALADIN_CHAINS = {
    "Spine":    ("mixamorig_spine", "mixamorig_spine2"),
    "Head":     ("mixamorig_neck", "mixamorig_head"),
    "LeftArm":  ("mixamorig_leftshoulder", "mixamorig_lefthand"),
    "RightArm": ("mixamorig_rightshoulder", "mixamorig_righthand"),
    "LeftLeg":  ("mixamorig_leftupleg", "mixamorig_lefttoebase"),
    "RightLeg": ("mixamorig_rightupleg", "mixamorig_righttoebase"),
    # Fingers matter here: this is a sword game, and the grip reads badly if the
    # hands are left in the target's rest pose.
    "LeftThumb":  ("mixamorig_lefthandthumb1", "mixamorig_lefthandthumb4"),
    "LeftIndex":  ("mixamorig_lefthandindex1", "mixamorig_lefthandindex4"),
    "LeftMiddle": ("mixamorig_lefthandmiddle1", "mixamorig_lefthandmiddle4"),
    "LeftRing":   ("mixamorig_lefthandring1", "mixamorig_lefthandring4"),
    "LeftPinky":  ("mixamorig_lefthandpinky1", "mixamorig_lefthandpinky4"),
    "RightThumb":  ("mixamorig_righthandthumb1", "mixamorig_righthandthumb4"),
    "RightIndex":  ("mixamorig_righthandindex1", "mixamorig_righthandindex4"),
    "RightMiddle": ("mixamorig_righthandmiddle1", "mixamorig_righthandmiddle4"),
    "RightRing":   ("mixamorig_righthandring1", "mixamorig_righthandring4"),
    "RightPinky":  ("mixamorig_righthandpinky1", "mixamorig_righthandpinky4"),
}

MANNY_CHAINS = {
    "Spine":    ("spine_01", "spine_05"),
    "Head":     ("neck_01", "head"),
    "LeftArm":  ("clavicle_l", "hand_l"),
    "RightArm": ("clavicle_r", "hand_r"),
    "LeftLeg":  ("thigh_l", "ball_l"),
    "RightLeg": ("thigh_r", "ball_r"),
    "LeftThumb":  ("thumb_01_l", "thumb_03_l"),
    "LeftIndex":  ("index_01_l", "index_03_l"),
    "LeftMiddle": ("middle_01_l", "middle_03_l"),
    "LeftRing":   ("ring_01_l", "ring_03_l"),
    "LeftPinky":  ("pinky_01_l", "pinky_03_l"),
    "RightThumb":  ("thumb_01_r", "thumb_03_r"),
    "RightIndex":  ("index_01_r", "index_03_r"),
    "RightMiddle": ("middle_01_r", "middle_03_r"),
    "RightRing":   ("ring_01_r", "ring_03_r"),
    "RightPinky":  ("pinky_01_r", "pinky_03_r"),
}

PALADIN_ROOT = "mixamorig_hips"
MANNY_ROOT = "pelvis"


def make_asset(package, factory_class, asset_class):
    """Loads the asset if it exists, otherwise creates it.

    Deliberately NOT delete-then-create: deleting and immediately recreating at
    the same path leaves the package half-torn-down, and the freshly created
    asset comes back without a working controller. Reusing and resetting is both
    safer and idempotent.
    """
    if unreal.EditorAssetLibrary.does_asset_exist(package):
        existing = unreal.EditorAssetLibrary.load_asset(package)
        if existing is not None:
            return existing

    path, name = package.rsplit("/", 1)
    return unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, path, asset_class, factory_class())


def bone_exists(controller, bone):
    """The controller has no bone list, so probe the reference pose instead."""
    try:
        controller.get_ref_pose_transform_of_bone(bone)
        return True
    except Exception:
        return False


def build_rig(package, mesh_path, chains, root_bone, label, problems):
    mesh = unreal.EditorAssetLibrary.load_asset(mesh_path)
    if mesh is None:
        problems.append("{}: skeletal mesh not found at {}".format(label, mesh_path))
        return None

    rig = make_asset(package, unreal.IKRigDefinitionFactory, unreal.IKRigDefinition)
    if rig is None:
        problems.append("{}: could not create or load the IK rig asset".format(label))
        return None

    controller = unreal.IKRigController.get_controller(rig)
    if controller is None:
        problems.append("{}: no IK rig controller".format(label))
        return None

    controller.set_skeletal_mesh(mesh)

    # Re-running must not stack duplicate chains on top of the old ones.
    for existing in list(controller.get_retarget_chains()):
        controller.remove_retarget_chain(existing.chain_name)

    if bone_exists(controller, root_bone):
        controller.set_retarget_root(root_bone)
    else:
        problems.append("{}: retarget root '{}' not on this skeleton".format(label, root_bone))

    added = 0
    for chain_name, (start, end) in chains.items():
        missing = [b for b in (start, end) if not bone_exists(controller, b)]
        if missing:
            problems.append("{}: chain '{}' skipped, missing {}".format(label, chain_name, missing))
            continue
        controller.add_retarget_chain(chain_name, start, end, "None")
        added += 1

    unreal.EditorAssetLibrary.save_asset(package)
    unreal.log("  {:<12} {:>2}/{} chains, root '{}'".format(label, added, len(chains), root_bone))
    return rig


def run():
    problems = []
    unreal.log("=" * 74)
    unreal.log("Retarget: paladin -> UE5 mannequin")

    paladin_rig = build_rig(IK_PALADIN, PALADIN_MESH, PALADIN_CHAINS,
                            PALADIN_ROOT, "IK_Paladin", problems)
    manny_rig = build_rig(IK_MANNY, MANNY_MESH, MANNY_CHAINS,
                          MANNY_ROOT, "IK_Manny", problems)

    if paladin_rig is None or manny_rig is None:
        for p in problems:
            unreal.log_error("  " + p)
        return

    # --- Retargeter --------------------------------------------------------
    retargeter = make_asset(RETARGETER, unreal.IKRetargetFactory, unreal.IKRetargeter)
    rc = unreal.IKRetargeterController.get_controller(retargeter)
    rc.set_ik_rig(unreal.RetargetSourceOrTarget.SOURCE, paladin_rig)
    rc.set_ik_rig(unreal.RetargetSourceOrTarget.TARGET, manny_rig)

    # UE 5.6 replaced the old solver stack with RetargetOps, and an asset made
    # through the factory starts with NO ops at all. Chain mapping lives on the
    # ops, so without this auto_map_chains silently maps nothing and every clip
    # retargets to a T-pose.
    rc.remove_all_ops()  # idempotent across re-runs
    rc.add_default_ops()
    unreal.log("  retarget ops: {}".format(rc.get_num_retarget_ops()))

    rc.auto_map_chains(unreal.AutoMapChainType.EXACT, True)

    # The step most likely to quietly ruin every clip: a T-pose source retargeted
    # onto an A-pose target without alignment gives permanently splayed arms.
    try:
        rc.auto_align_all_bones(unreal.RetargetSourceOrTarget.SOURCE,
                                unreal.RetargetAutoAlignMethod.CHAIN_TO_CHAIN)
        unreal.log("  auto-aligned source pose (chain to chain)")
    except Exception as error:
        problems.append("auto-align failed: {}".format(error))

    unreal.EditorAssetLibrary.save_asset(RETARGETER)

    mapped = 0
    for chain in PALADIN_CHAINS:
        try:
            if str(rc.get_source_chain(chain)) not in ("None", ""):
                mapped += 1
        except Exception:
            pass
    unreal.log("  chains mapped: {}/{}".format(mapped, len(PALADIN_CHAINS)))

    # --- Batch retarget ----------------------------------------------------
    reg = unreal.AssetRegistryHelpers.get_asset_registry()
    # The batch operation takes AssetData, not loaded AnimSequence objects, so
    # filter on the registry's class path rather than loading and isinstance-ing.
    sequences = [data for data in reg.get_assets_by_path(ANIM_SEARCH_DIR, recursive=True)
                 if str(data.asset_class_path.asset_name) == "AnimSequence"]

    unreal.log("  sequences to retarget: {}".format(len(sequences)))

    paladin_mesh = unreal.EditorAssetLibrary.load_asset(PALADIN_MESH)
    manny_mesh = unreal.EditorAssetLibrary.load_asset(MANNY_MESH)

    # duplicate_and_retarget is deprecated in 5.8 in favour of the struct form.
    inputs = unreal.IKRetargetBatchOperationInputs()
    inputs.set_editor_property("assets_to_retarget", sequences)
    inputs.set_editor_property("source_mesh", paladin_mesh)
    inputs.set_editor_property("target_mesh", manny_mesh)
    inputs.set_editor_property("ik_retarget_asset", retargeter)
    inputs.set_editor_property("search", "A_")
    inputs.set_editor_property("replace", "A_Manny_")
    inputs.set_editor_property("target_path", RETARGET_DEST)
    inputs.set_editor_property("use_source_path", False)
    inputs.set_editor_property("include_referenced_assets", True)
    inputs.set_editor_property("overwrite_existing_files", True)

    try:
        unreal.IKRetargetBatchOperation.run_batch_retarget(inputs)
    except Exception as error:
        problems.append("batch retarget failed: {}".format(error))

    unreal.EditorAssetLibrary.save_directory("/Game/ARPG/Animations", only_if_is_dirty=False)

    # --- Report ------------------------------------------------------------
    produced = []
    for data in reg.get_assets_by_path(RETARGET_DEST, recursive=True):
        asset = unreal.EditorAssetLibrary.load_asset(str(data.package_name))
        if isinstance(asset, unreal.AnimSequence):
            produced.append(asset)

    # Retargeted output lands beside the source unless told otherwise; find it
    # wherever it went.
    if not produced:
        for data in reg.get_assets_by_path("/Game/ARPG/Animations", recursive=True):
            name = str(data.asset_name)
            if name.startswith("A_Manny_"):
                asset = unreal.EditorAssetLibrary.load_asset(str(data.package_name))
                if isinstance(asset, unreal.AnimSequence):
                    produced.append(asset)

    unreal.log("  retargeted sequences: {}".format(len(produced)))
    for seq in sorted(produced, key=lambda s: s.get_name())[:6]:
        skel = seq.get_editor_property("skeleton")
        unreal.log("      {:<38} skeleton={}".format(seq.get_name(), skel.get_name() if skel else "?"))
    if len(produced) > 6:
        unreal.log("      ... and {} more".format(len(produced) - 6))

    if problems:
        unreal.log_warning("  PROBLEMS:")
        for p in problems:
            unreal.log_warning("      {}".format(p))
    unreal.log("=" * 74)


run()
