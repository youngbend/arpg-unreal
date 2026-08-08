"""
Imports Mixamo animations and retargets them onto the UE5 mannequin, leaving
AnimSequences on the mannequin skeleton with driven root motion -- ready to be
cut into montages.

    UnrealEditor-Cmd.exe <project>.uproject ^
        -ExecutePythonScript="<abs>/Tools/import_mixamo_animation.py <source>" ^
        -unattended -nullrhi -nosplash

The script path must be ABSOLUTE. -ExecutePythonScript resolves a relative path
against Engine/Binaries/Win64, not the project, and reports only "Could not load
Python file" when it does not find it there.

<source> is a .fbx file, or a directory of them. Options are listed in OPTIONS
below; put them BEFORE the source path.

    ...import_mixamo_animation.py C:/Users/young/Downloads/sns_raw
    ...import_mixamo_animation.py --dest /Game/ARPG/Animations/Sword C:/clips

Unreal splits the -ExecutePythonScript string on spaces, so a source path
containing spaces arrives as several arguments. The remaining arguments are
re-joined and tested as one path before being treated as a list, which makes
"C:/My Clips/Sword And Shield Slash.fbx" work as long as it is last.


ABOUT THE ROOT BONE
-------------------
A Mixamo rig has no root bone. Travel lives on mixamorig:Hips, which is why a
straight import gives you an animation that cannot drive a character.

The fix is not to graft a root bone onto the Mixamo skeleton. The mannequin
already HAS a root bone -- it just sits at the origin doing nothing, because
nothing on the source side maps to it. What this script does is configure the
retargeter's Root Motion op to synthesise that track: RootMotionSource is set to
GenerateFromTargetPelvis, so once the pose has been retargeted, the root is
placed under the mannequin's pelvis each frame and flattened to Z=0. The result
is a real animated root track on the output sequence.

AddDefaultOps() already puts a Root Motion op in the stack, but it defaults to
CopyFromSourceRoot -- which on a Mixamo source copies from a bone that either
does not exist or never moves, and quietly produces a root that stays at the
origin. Configuring it is the whole point of this script.

Root motion is then enabled on the output sequences, so a montage made from them
moves the character. Pass --no-root-motion for clips you want to stay in place.


OTHER THINGS THAT SILENTLY GO WRONG
-----------------------------------
Chain names ARE the mapping. A chain that resolves on one rig but not the other
is not an error -- the limb just never moves. So chains are resolved against
both skeletons first and only the intersection is created, with the dropped ones
reported.

Bone naming varies between Mixamo downloads: the ':' in 'mixamorig:Hips' is
sanitised to '_' on import, but some exports have no prefix at all and some use
'mixamorig1'. The prefix is discovered by probing rather than assumed.

Scale is NOT handled by the FBX importer the way it is for glTF. Mixamo FBX
commonly lands at 1/100. The rig is measured against the mannequin's ~180uu and
re-imported at 100x if it came in tiny; --scale overrides the guess.

Interchange ignores AssetImportTask.options. Pipelines have to go through
ImportAssetParameters.override_pipelines or the settings are a silent no-op.
"""

import os
import re
import sys

import unreal


# --- OPTIONS ---------------------------------------------------------------

DEFAULTS = {
    "--work":        "/Game/ARPG/Animations/Mixamo",
    "--dest":        "/Game/ARPG/Animations/Mannequin",
    "--target-mesh": "/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple",
    "--rig":         "",       # reuse an already-imported Mixamo skeletal mesh
    "--prefix":      "A_",     # name prefix on the retargeted sequences
    "--scale":       "",       # import uniform scale; blank = measure and guess
    "--root-height": "ground",  # ground | source
    "--align":       "chain",  # chain | mesh | arms | none | keep, see align_source_pose
}

SWITCHES = {
    "--no-root-motion":    False,  # leave EnableRootMotion off on the output
    "--rotate-with-pelvis": False,  # let the root yaw with the pelvis
    "--reimport-rig":      False,  # re-import the source rig even if one exists
}

WORK_PREFIX = "A_Mixamo_"


# --- CHAIN TEMPLATES -------------------------------------------------------
# chain name -> (start candidates, end candidates), first that resolves wins.
# The keys are what pair the two rigs together, so they must match across both
# tables -- that name match IS the retarget mapping.

MIXAMO_CHAINS = {
    "Spine":       (["Spine"],         ["Spine2", "Spine1", "Spine"]),
    "Head":        (["Neck"],          ["Head"]),
    "LeftArm":     (["LeftShoulder"],  ["LeftHand"]),
    "RightArm":    (["RightShoulder"], ["RightHand"]),
    "LeftLeg":     (["LeftUpLeg"],     ["LeftToeBase", "LeftFoot"]),
    "RightLeg":    (["RightUpLeg"],    ["RightToeBase", "RightFoot"]),
    # Fingers are worth mapping for a weapon game: without them the hands fall
    # back to the target's rest pose and the grip reads as an open palm.
    #
    # END AT BONE 3, NOT 4. Mixamo fingers are Thumb1..Thumb4 where Thumb4 is
    # the fingertip end marker; the mannequin has three. Ending at 4 makes a
    # 4-bone chain map onto a 3-bone one, and the retargeter then distributes
    # rotation by normalised position along the chain instead of joint to
    # joint. Measured against these clips that injected ~50 degrees of curl
    # into fingers whose source bones do not rotate at all.
    "LeftThumb":   (["LeftHandThumb1"],  ["LeftHandThumb3", "LeftHandThumb2"]),
    "LeftIndex":   (["LeftHandIndex1"],  ["LeftHandIndex3", "LeftHandIndex2"]),
    "LeftMiddle":  (["LeftHandMiddle1"], ["LeftHandMiddle3", "LeftHandMiddle2"]),
    "LeftRing":    (["LeftHandRing1"],   ["LeftHandRing3", "LeftHandRing2"]),
    "LeftPinky":   (["LeftHandPinky1"],  ["LeftHandPinky3", "LeftHandPinky2"]),
    "RightThumb":  (["RightHandThumb1"],  ["RightHandThumb3", "RightHandThumb2"]),
    "RightIndex":  (["RightHandIndex1"],  ["RightHandIndex3", "RightHandIndex2"]),
    "RightMiddle": (["RightHandMiddle1"], ["RightHandMiddle3", "RightHandMiddle2"]),
    "RightRing":   (["RightHandRing1"],   ["RightHandRing3", "RightHandRing2"]),
    "RightPinky":  (["RightHandPinky1"],  ["RightHandPinky3", "RightHandPinky2"]),
}

# Chains whose bones correspond joint for joint once the fingertip is excluded:
# arms 4-4, legs 4-4, fingers 3-3. These get One To One rotation, which copies
# each joint's rotation straight across. Spine (3 vs 5) and Head (2 vs 3)
# genuinely differ in bone count and keep Interpolated, which is what that mode
# exists for.
ONE_TO_ONE_CHAINS = {
    "LeftArm", "RightArm", "LeftLeg", "RightLeg",
    "LeftThumb", "LeftIndex", "LeftMiddle", "LeftRing", "LeftPinky",
    "RightThumb", "RightIndex", "RightMiddle", "RightRing", "RightPinky",
}

# Only these chains get their source retarget pose auto-aligned. A Mixamo rig is
# T-posed and the mannequin is A-posed, and that difference lives entirely in
# the arms -- aligning the legs and extremities as well rotates their rest pose
# away from where it should be, which then rides under every frame as a constant
# offset. Measured: aligning everything left ankles and knees sitting 2.0-2.5x
# further from rest than the source, while their motion tracked correctly.
ALIGN_CHAINS = {"Spine", "Head", "LeftArm", "RightArm"}

# Spine and foot ends differ between the UE4 and UE5 mannequins, hence the
# candidate lists rather than fixed names.
MANNY_CHAINS = {
    "Spine":       (["spine_01"],   ["spine_05", "spine_04", "spine_03"]),
    "Head":        (["neck_01", "neck"], ["head"]),
    "LeftArm":     (["clavicle_l"], ["hand_l"]),
    "RightArm":    (["clavicle_r"], ["hand_r"]),
    "LeftLeg":     (["thigh_l"],    ["ball_l", "foot_l"]),
    "RightLeg":    (["thigh_r"],    ["ball_r", "foot_r"]),
    "LeftThumb":   (["thumb_01_l"],  ["thumb_03_l"]),
    "LeftIndex":   (["index_01_l"],  ["index_03_l"]),
    "LeftMiddle":  (["middle_01_l"], ["middle_03_l"]),
    "LeftRing":    (["ring_01_l"],   ["ring_03_l"]),
    "LeftPinky":   (["pinky_01_l"],  ["pinky_03_l"]),
    "RightThumb":  (["thumb_01_r"],  ["thumb_03_r"]),
    "RightIndex":  (["index_01_r"],  ["index_03_r"]),
    "RightMiddle": (["middle_01_r"], ["middle_03_r"]),
    "RightRing":   (["ring_01_r"],   ["ring_03_r"]),
    "RightPinky":  (["pinky_01_r"],  ["pinky_03_r"]),
}

# Unreal sanitises ':' to '_'; older and re-exported rigs vary from there.
MIXAMO_PREFIXES = ["mixamorig_", "mixamorig1_", "mixamorig:", "mixamorig", ""]

MANNY_PELVIS = "pelvis"
MANNY_ROOT = "root"

MANNEQUIN_HEIGHT_UU = 180.0


# --- ARGUMENTS -------------------------------------------------------------

def parse_arguments(argv):
    """Hand-rolled so a source path containing spaces survives.

    argparse would treat each space-separated fragment as its own positional and
    there is no way to tell it otherwise, because Unreal has already thrown the
    original quoting away by the time we see sys.argv.
    """
    options = dict(DEFAULTS)
    switches = dict(SWITCHES)
    rest = []

    index = 0
    while index < len(argv):
        token = argv[index]
        if token in switches:
            switches[token] = True
            index += 1
        elif token in options:
            if index + 1 >= len(argv):
                unreal.log_error("{} needs a value.".format(token))
                return None, None, None
            options[token] = argv[index + 1]
            index += 2
        elif token.startswith("--"):
            unreal.log_error("Unknown option {}.".format(token))
            return None, None, None
        else:
            rest.append(token)
            index += 1

    return options, switches, rest


def collect_sources(tokens):
    """Turns the leftover arguments into a list of .fbx paths."""
    def expand(path):
        if os.path.isdir(path):
            return [os.path.join(path, name) for name in sorted(os.listdir(path))
                    if name.lower().endswith(".fbx")]
        if os.path.isfile(path) and path.lower().endswith(".fbx"):
            return [path]
        return []

    if not tokens:
        return [], ["no source given"]

    # A spaced path arrives split; re-joining is the common case, so try it first.
    joined = " ".join(tokens)
    if os.path.exists(joined):
        found = expand(joined)
        return found, [] if found else ["no .fbx under {}".format(joined)]

    found, missing = [], []
    for token in tokens:
        matched = expand(token)
        if matched:
            found.extend(matched)
        else:
            missing.append(token)
    return found, missing


def sanitise(stem):
    """'Sword And Shield Attack(1)' -> 'SwordAndShieldAttack_1'

    Browser downloads collide on name and get a '(n)' suffix, and parentheses
    and spaces are not legal in an Unreal asset name.
    """
    text = re.sub(r"\((\d+)\)", r" _\1", stem)
    words = []
    for part in re.split(r"[^A-Za-z0-9_]+", text):
        if not part:
            continue
        words.append(part if part.startswith("_") else part[0].upper() + part[1:])
    name = "".join(words)
    if not name:
        return "Clip"
    return "Clip" + name if name[0].isdigit() else name


# --- IMPORT ----------------------------------------------------------------

def build_pipeline(scale, skeleton=None):
    """Skeleton None means 'import the rig'; otherwise animation-only."""
    pipeline = unreal.InterchangeGenericAssetsPipeline()
    pipeline.set_editor_property("import_offset_uniform_scale", scale)

    common = pipeline.get_editor_property(
        "common_skeletal_meshes_and_animations_properties")
    common.set_editor_property("import_only_animations", skeleton is not None)
    if skeleton is not None:
        common.set_editor_property("skeleton", skeleton)

    mesh = pipeline.get_editor_property("mesh_pipeline")
    mesh.set_editor_property("import_skeletal_meshes", skeleton is None)
    mesh.set_editor_property("import_static_meshes", False)

    anim = pipeline.get_editor_property("animation_pipeline")
    anim.set_editor_property("import_animations", skeleton is not None)
    anim.set_editor_property("import_bone_tracks", True)

    # Mixamo "with skin" downloads carry the character's materials and textures.
    # We want them once with the rig at most, never 13 more times with the clips.
    material = pipeline.get_editor_property("material_pipeline")
    material.set_editor_property("import_materials", skeleton is None)

    return pipeline


def run_import(destination, filename, pipeline):
    params = unreal.ImportAssetParameters()
    params.set_editor_property("is_automated", True)
    params.set_editor_property("replace_existing", True)
    # The pipeline object is transient; its path resolves for the duration of
    # this call, which is all the importer needs.
    params.set_editor_property(
        "override_pipelines", [unreal.SoftObjectPath(pipeline.get_path_name())])

    source = unreal.InterchangeManager.create_source_data(filename)
    manager = unreal.InterchangeManager.get_interchange_manager_scripted()
    manager.import_asset(destination, source, params)


def assets_of_type(package_path, asset_type):
    found = []
    for path in unreal.EditorAssetLibrary.list_assets(package_path, recursive=True) or []:
        asset = unreal.EditorAssetLibrary.load_asset(path.split(".")[0])
        if isinstance(asset, asset_type):
            found.append(asset)
    return found


def import_rig(rig_dir, filename, scale, problems):
    """Imports the character mesh that carries the skeleton to retarget from.

    Mixamo "with skin" downloads all contain the same character, so any one of
    them can supply the rig.
    """
    run_import(rig_dir, filename, build_pipeline(scale))
    meshes = assets_of_type(rig_dir, unreal.SkeletalMesh)

    if not meshes:
        problems.append("no skeletal mesh came out of {} -- is this a "
                        "'without skin' download? Pass --rig to reuse an "
                        "existing one.".format(os.path.basename(filename)))
        return None
    if len(meshes) > 1:
        unreal.log_warning("  {} skeletal meshes imported; using '{}'".format(
            len(meshes), meshes[0].get_name()))
    return meshes[0]


def mesh_height(mesh):
    return mesh.get_bounds().box_extent.z * 2.0


def import_clips(clips_dir, filenames, skeleton, scale, problems):
    """Imports each clip animation-only against the source skeleton.

    Unreal names imported sequences <filestem><animname> with no separator, so
    each file goes into a scratch folder of its own and the single sequence it
    produces is renamed out of it. Doing this per-file keeps a clip whose take
    is named the same as another's from colliding before it can be renamed.
    """
    pipeline = build_pipeline(scale, skeleton)
    sequences = []

    for filename in filenames:
        stem = os.path.splitext(os.path.basename(filename))[0]
        name = sanitise(stem)
        scratch = "{}/_import/{}".format(clips_dir, name)

        run_import(scratch, filename, pipeline)
        imported = assets_of_type(scratch, unreal.AnimSequence)

        if not imported:
            problems.append("{}: imported no animation".format(stem))
            continue
        if len(imported) > 1:
            unreal.log_warning("  {}: {} takes in one file, suffixing".format(
                stem, len(imported)))

        for index, sequence in enumerate(imported):
            desired = WORK_PREFIX + name
            if len(imported) > 1:
                desired = "{}_{}".format(desired, index + 1)

            target = "{}/{}".format(clips_dir, desired)
            source_path = sequence.get_path_name().split(".")[0]

            if unreal.EditorAssetLibrary.does_asset_exist(target):
                unreal.EditorAssetLibrary.delete_asset(target)
            if unreal.EditorAssetLibrary.rename_asset(source_path, target):
                sequence = unreal.EditorAssetLibrary.load_asset(target)

            sequences.append(sequence)

    # Everything worth keeping has been renamed out; what is left is the
    # per-file scratch folders this function made.
    scratch_root = clips_dir + "/_import"
    if unreal.EditorAssetLibrary.does_directory_exist(scratch_root):
        unreal.EditorAssetLibrary.delete_directory(scratch_root)
    return sequences


# --- RIGS ------------------------------------------------------------------

def make_asset(package, factory_class, asset_class):
    """Loads the asset if it exists, otherwise creates it.

    Deliberately NOT delete-then-create: deleting and immediately recreating at
    the same path leaves the package half-torn-down, and the freshly created
    asset comes back without a working controller.
    """
    if unreal.EditorAssetLibrary.does_asset_exist(package):
        existing = unreal.EditorAssetLibrary.load_asset(package)
        if existing is not None:
            return existing

    path, name = package.rsplit("/", 1)
    return unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, path, asset_class, factory_class())


def bone_map(mesh, problems):
    """Lowercased bone name -> the skeleton's own spelling.

    IKRigController.GetRefPoseTransformOfBone cannot be used to test whether a
    bone exists: for an unknown name it logs a warning and hands back the
    identity transform rather than failing, so a probe built on it reports every
    bone as present and every chain gets created with whatever was guessed.
    The reference pose carries the real list.
    """
    skeleton = mesh.get_editor_property("skeleton")
    try:
        pose = unreal.AnimPoseExtensions.get_reference_pose(skeleton)
        names = unreal.AnimPoseExtensions.get_bone_names(pose)
    except Exception as error:
        problems.append("could not read the bones of {}: {}".format(
            skeleton.get_name() if skeleton else "?", error))
        return {}
    return dict((str(name).lower(), str(name)) for name in names)


def first_present(bones, candidates):
    """Returns the skeleton's spelling of the first candidate it has."""
    for candidate in candidates:
        actual = bones.get(candidate.lower())
        if actual:
            return actual
    return None


def find_mixamo_prefix(bones):
    for prefix in MIXAMO_PREFIXES:
        if (prefix + "Hips").lower() in bones:
            return prefix
    return None


def resolve_chains(bones, template, prefix=""):
    """Chain name -> (start, end) for the chains this skeleton actually has."""
    resolved = {}
    for chain, (starts, ends) in template.items():
        start = first_present(bones, [prefix + b for b in starts])
        end = first_present(bones, [prefix + b for b in ends])
        if start and end:
            resolved[chain] = (start, end)
    return resolved


def chain_bones(clip, start, end):
    """Every bone from start to end inclusive, walked up the hierarchy.

    There is no Python API for a skeleton's parent table, but an AnimSequence on
    that skeleton can walk it, and by this point one exists.
    """
    try:
        path = [str(b) for b in
                unreal.AnimationLibrary.find_bone_path_to_root(clip, end)]
    except Exception:
        return []
    walked = []
    for bone in path:
        walked.append(bone)
        if bone.lower() == start.lower():
            return list(reversed(walked))
    return []


def build_rig(package, mesh, chains, pelvis, root_motion_bone, label, problems):
    rig = make_asset(package, unreal.IKRigDefinitionFactory, unreal.IKRigDefinition)
    if rig is None:
        problems.append("{}: could not create or load the IK rig".format(label))
        return None

    controller = unreal.IKRigController.get_controller(rig)
    if controller is None:
        problems.append("{}: no IK rig controller".format(label))
        return None

    controller.set_skeletal_mesh(mesh)

    # Re-running must not stack duplicate chains on top of the old ones.
    for existing in list(controller.get_retarget_chains()):
        controller.remove_retarget_chain(existing.chain_name)

    for chain, (start, end) in chains.items():
        controller.add_retarget_chain(chain, start, end, "None")

    # set_retarget_root is the pelvis -- the bone that translates the character.
    # The root motion bone is separate and is what the Root Motion op writes to.
    controller.set_retarget_root(pelvis)
    if root_motion_bone:
        controller.set_root_motion_bone(root_motion_bone)

    unreal.EditorAssetLibrary.save_asset(package)
    unreal.log("  {:<12} {:>2} chains, pelvis '{}', root '{}'".format(
        label, len(chains), pelvis, root_motion_bone or "-"))
    return rig


# --- RETARGET --------------------------------------------------------------

def enum_value(names, entry):
    """Resolves an enum entry, tolerating the 'E' prefix being kept or stripped.

    Python normally exposes ERootMotionSource as unreal.RootMotionSource, but
    that is a naming convention rather than a guarantee, and getting it wrong
    would disable root motion for every clip with only a stack trace to explain.
    """
    for name in names:
        enum_type = getattr(unreal, name, None)
        if enum_type is not None and hasattr(enum_type, entry):
            return getattr(enum_type, entry)
    raise AttributeError("no enum {} with entry {}".format(names, entry))


def configure_root_motion(rc, target_root, target_pelvis, options, switches, problems):
    """Points the Root Motion op at the mannequin's root and drives it.

    AddDefaultOps() supplies the op but leaves it on CopyFromSourceRoot, which
    reads a Mixamo source bone that does not move. Without this the output has a
    root track pinned at the origin and no amount of ticking EnableRootMotion on
    the sequence will make the character travel.
    """
    controller = None
    for index in range(rc.get_num_retarget_ops()):
        candidate = rc.get_op_controller(index)
        # Identify by API rather than class name, but on set_target_root_bone
        # specifically: the Pelvis Motion op that AddDefaultOps also adds has a
        # set_target_pelvis_bone too, and matching on that picks the wrong op.
        if candidate is not None and hasattr(candidate, "set_target_root_bone"):
            controller = candidate
            break

    if controller is None:
        problems.append("no Root Motion op in the stack -- output will have no "
                        "root motion")
        return False
    if target_root is None:
        return False

    controller.set_target_root_bone(target_root)
    controller.set_target_pelvis_bone(target_pelvis)

    try:
        settings = controller.get_settings()
        settings.set_editor_property(
            "root_motion_source",
            enum_value(["RootMotionSource", "ERootMotionSource"],
                       "GENERATE_FROM_TARGET_PELVIS"))
        settings.set_editor_property(
            "root_height_source",
            enum_value(["RootMotionHeightSource", "ERootMotionHeightSource"],
                       "SNAP_TO_GROUND" if options["--root-height"] == "ground"
                       else "COPY_HEIGHT_FROM_SOURCE"))
        settings.set_editor_property("maintain_offset_from_pelvis", True)
        settings.set_editor_property("propagate_to_non_retargeted_children", True)
        settings.set_editor_property("rotate_with_pelvis",
                                     switches["--rotate-with-pelvis"])
        controller.set_settings(settings)
    except Exception as error:
        problems.append("could not configure the Root Motion op: {}".format(error))
        return False

    unreal.log("  root motion : '{}' -> '{}', height {}".format(
        target_pelvis, target_root, options["--root-height"]))
    return True


def configure_fk_chains(rc, chains, problems):
    """Sets One To One rotation on the chains that map joint for joint.

    The default is Interpolated, which spreads the source chain's rotation along
    the target chain by normalised position. That is the right behaviour when
    the bone counts differ and the wrong one when they match -- it smears each
    joint's rotation into its neighbours instead of copying it.
    """
    controller = None
    for index in range(rc.get_num_retarget_ops()):
        candidate = rc.get_op_controller(index)
        if candidate is None:
            continue
        try:
            settings = candidate.get_settings()
        except Exception:
            continue
        # The FK chains op is the one carrying per-chain settings.
        if settings is not None and hasattr(settings, "chains_to_retarget"):
            controller = candidate
            break

    if controller is None:
        problems.append("no FK Chains op found; chains keep default rotation mode")
        return 0

    settings = controller.get_settings()
    chain_settings = settings.get_editor_property("chains_to_retarget")

    changed = 0
    for entry in chain_settings:
        name = str(entry.get_editor_property("target_chain_name"))
        if name not in chains:
            continue
        try:
            entry.set_editor_property(
                "rotation_mode",
                enum_value(["FKChainRotationMode", "EFKChainRotationMode"],
                           "ONE_TO_ONE"))
            changed += 1
        except Exception as error:
            problems.append("could not set rotation mode on {}: {}".format(name, error))
            break

    settings.set_editor_property("chains_to_retarget", chain_settings)
    controller.set_settings(settings)
    return changed


def align_source_pose(rc, clip, source_chains, strategy, problems):
    """Poses the source skeleton to match the target's, and says what it did.

    THIS IS THE STEP EVERYTHING ELSE DEPENDS ON. Retargeting transfers each
    joint's rotation relative to the retarget pose, so if the two characters do
    not stand the same way in that pose, every clip inherits the difference as a
    constant offset. Open the retargeter and the two skeletons should overlap;
    if they are holding visibly different poses, this is why.

    Strategies:
      chain - align all bones from chain directions. The engine's own default.
      mesh  - align all bones from skinned mesh direction instead. Worth trying
              if hands or feet look wrong, since a skeleton gives no reliable
              direction for leaf bones.
      arms  - align only ALIGN_CHAINS, on the theory that T-pose vs A-pose is an
              arms problem. Leaves every other bone at whatever relative
              orientation the two rigs happen to have.
      none  - no alignment at all.
      keep  - touch nothing. Use this once the retarget pose has been fixed by
              hand in the retargeter, or a re-run will discard that work.

    NO AUTOMATIC ALIGNMENT IS GUARANTEED TO BE RIGHT. Whether the two characters
    actually end up standing the same way is a judgement made by eye, in the
    retargeter, on the source-vs-target preview. If they hold visibly different
    poses there, fix it with Edit Pose on the source and then re-run with
    --align keep.

    The retarget pose is PERSISTENT STATE on the retargeter asset, and this
    script reuses that asset across runs. Aligning a narrower set of bones does
    not undo a previous run's wider alignment -- offsets on bones no longer
    being touched stay exactly where they were, and the run appears to change
    nothing. So the pose is cleared before every re-align, which is exactly why
    'keep' has to exist.
    """
    if strategy == "keep":
        return "kept (existing retarget pose left untouched)"

    try:
        pose = rc.get_current_retarget_pose_name(unreal.RetargetSourceOrTarget.SOURCE)
        rc.reset_retarget_pose(pose, [], unreal.RetargetSourceOrTarget.SOURCE)
    except Exception as error:
        problems.append("could not reset the source retarget pose, so any "
                        "earlier alignment is still applied: {}".format(error))

    if strategy == "none":
        return "none"

    source = unreal.RetargetSourceOrTarget.SOURCE
    try:
        if strategy == "arms":
            bones = []
            for chain in sorted(ALIGN_CHAINS & set(source_chains)):
                start, end = source_chains[chain]
                bones.extend(chain_bones(clip, start, end))
            if not bones:
                problems.append("no bones resolved to align")
                return "failed"
            rc.auto_align_bones(
                bones, unreal.RetargetAutoAlignMethod.CHAIN_TO_CHAIN, source)
            return "{} bones (chain-to-chain)".format(len(bones))

        method = enum_value(["RetargetAutoAlignMethod"],
                            "MESH_TO_MESH" if strategy == "mesh" else "CHAIN_TO_CHAIN")
        rc.auto_align_all_bones(source, method)
        return "all bones ({})".format(strategy)
    except Exception as error:
        problems.append("auto-align failed: {}".format(error))
        return "failed"


def build_retargeter(package, source_rig, target_rig, target_root, target_pelvis,
                     source_clip, source_chains, options, switches, problems):
    retargeter = make_asset(package, unreal.IKRetargetFactory, unreal.IKRetargeter)
    rc = unreal.IKRetargeterController.get_controller(retargeter)

    rc.set_ik_rig(unreal.RetargetSourceOrTarget.SOURCE, source_rig)
    rc.set_ik_rig(unreal.RetargetSourceOrTarget.TARGET, target_rig)

    # UE 5.6 replaced the old solver stack with RetargetOps, and an asset made
    # through the factory starts with NO ops at all. Chain mapping lives on the
    # ops, so without this auto_map_chains silently maps nothing and every clip
    # retargets to a T-pose. The ops also have to be added AFTER the rigs, since
    # each op reads the rigs' pelvis and root when it joins the stack.
    rc.remove_all_ops()
    rc.add_default_ops()

    rc.auto_map_chains(unreal.AutoMapChainType.EXACT, True)
    configure_root_motion(rc, target_root, target_pelvis, options, switches, problems)

    one_to_one = configure_fk_chains(rc, ONE_TO_ONE_CHAINS, problems)
    unreal.log("  fk rotation : one-to-one on {} chains, interpolated on the "
               "rest".format(one_to_one))

    aligned = align_source_pose(rc, source_clip, source_chains,
                                options["--align"], problems)
    unreal.log("  pose align  : {}".format(aligned))

    unreal.EditorAssetLibrary.save_asset(package)
    return retargeter, rc


def batch_retarget(retargeter, sequences, clips_dir, source_mesh, target_mesh,
                   options, problems):
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    # The batch operation takes AssetData, not loaded AnimSequence objects, so
    # filter the registry on the class path rather than loading and isinstance-ing.
    wanted = set(s.get_name() for s in sequences)
    asset_data = [d for d in registry.get_assets_by_path(clips_dir, recursive=True)
                  if str(d.asset_class_path.asset_name) == "AnimSequence"
                  and str(d.asset_name) in wanted]

    inputs = unreal.IKRetargetBatchOperationInputs()
    inputs.set_editor_property("assets_to_retarget", asset_data)
    inputs.set_editor_property("source_mesh", source_mesh)
    inputs.set_editor_property("target_mesh", target_mesh)
    inputs.set_editor_property("ik_retarget_asset", retargeter)
    inputs.set_editor_property("search", WORK_PREFIX)
    inputs.set_editor_property("replace", options["--prefix"])
    inputs.set_editor_property("target_path", options["--dest"])
    inputs.set_editor_property("use_source_path", False)
    inputs.set_editor_property("include_referenced_assets", True)
    inputs.set_editor_property("overwrite_existing_files", True)

    try:
        unreal.IKRetargetBatchOperation.run_batch_retarget(inputs)
    except Exception as error:
        problems.append("batch retarget failed: {}".format(error))
        return []

    expected = set(options["--prefix"] + s.get_name()[len(WORK_PREFIX):]
                   for s in sequences)
    return [a for a in assets_of_type(options["--dest"], unreal.AnimSequence)
            if a.get_name() in expected]


def apply_root_motion_settings(sequence):
    sequence.set_editor_property("enable_root_motion", True)
    try:
        sequence.set_editor_property(
            "root_motion_root_lock",
            enum_value(["RootMotionRootLock", "ERootMotionRootLock"], "REF_POSE"))
    except Exception:
        pass  # the lock mode is a default, not the thing that makes this work
    sequence.set_editor_property("force_root_lock", False)


def root_travel(sequence, root_bone):
    """How far the root bone actually moves, in uu.

    This is the check that matters: it is the difference between a sequence that
    will drive a character through a montage and one that will slide in place.
    """
    frames = unreal.AnimationLibrary.get_num_frames(sequence)
    if frames < 2:
        return 0.0
    # Sampling rather than walking every frame: this runs per clip and we only
    # need to tell "moves" from "does not move", not measure a path length.
    step = max(1, frames // 24)
    try:
        start = unreal.AnimationLibrary.get_bone_pose_for_frame(
            sequence, root_bone, 0, False)
        furthest = 0.0
        for frame in list(range(step, frames, step)) + [frames - 1]:
            pose = unreal.AnimationLibrary.get_bone_pose_for_frame(
                sequence, root_bone, frame, False)
            distance = (pose.translation - start.translation).length()
            furthest = max(furthest, distance)
        return furthest
    except Exception:
        return -1.0


# --- MAIN ------------------------------------------------------------------

def run():
    options, switches, rest = parse_arguments(sys.argv[1:])
    if options is None:
        return

    filenames, missing = collect_sources(rest)
    if missing:
        for item in missing:
            unreal.log_error("Source not found: {}".format(item))
    if not filenames:
        unreal.log_error("Nothing to import. Give a .fbx or a folder of them.")
        return

    work = options["--work"].rstrip("/")
    rig_dir, clips_dir, rigs_dir = work + "/Rig", work + "/Clips", work + "/Rigs"
    problems = []

    unreal.log("=" * 74)
    unreal.log("Mixamo -> UE5 mannequin  ({} file{})".format(
        len(filenames), "" if len(filenames) == 1 else "s"))

    # --- source rig -------------------------------------------------------
    scale = float(options["--scale"]) if options["--scale"] else 1.0

    reused = False
    if options["--rig"]:
        source_mesh = unreal.EditorAssetLibrary.load_asset(options["--rig"])
        if not isinstance(source_mesh, unreal.SkeletalMesh):
            unreal.log_error("--rig is not a skeletal mesh: {}".format(options["--rig"]))
            return
        reused = True
    else:
        existing = assets_of_type(rig_dir, unreal.SkeletalMesh)
        if existing and not switches["--reimport-rig"]:
            source_mesh = existing[0]
            reused = True
            unreal.log("  rig         : reusing {}".format(source_mesh.get_name()))
        else:
            source_mesh = import_rig(rig_dir, filenames[0], scale, problems)
            if source_mesh is None:
                for problem in problems:
                    unreal.log_error("  " + problem)
                return

            # Metres-vs-centimetres only shows up after the fact, and retargeting
            # 40 clips off a 1.7uu character wastes the whole run.
            height = mesh_height(source_mesh)
            if not options["--scale"] and height < 20.0:
                scale = 100.0
                unreal.log_warning(
                    "  rig imported at {:.2f}uu; re-importing at 100x".format(height))
                source_mesh = import_rig(rig_dir, filenames[0], scale, problems)
                if source_mesh is None:
                    return

    unreal.log("  rig height  : {:.1f} uu (mannequin ~{:.0f})".format(
        mesh_height(source_mesh), MANNEQUIN_HEIGHT_UU))

    # Clips must be imported at the same scale the rig was, or their translation
    # tracks do not match the skeleton they are bound to. On a fresh import that
    # is handled above; on a reused rig the original scale is not recorded
    # anywhere we can read it back from, so say so rather than guess.
    if reused and not options["--scale"]:
        unreal.log_warning("  reusing a rig imported by an earlier run: clips "
                           "will import at scale 1.0. If that run scaled the "
                           "rig, pass the same --scale.")

    source_skeleton = source_mesh.get_editor_property("skeleton")

    target_mesh = unreal.EditorAssetLibrary.load_asset(options["--target-mesh"])
    if not isinstance(target_mesh, unreal.SkeletalMesh):
        unreal.log_error("Target mesh not found: {}".format(options["--target-mesh"]))
        return

    # --- clips ------------------------------------------------------------
    sequences = import_clips(clips_dir, filenames, source_skeleton, scale, problems)
    if not sequences:
        unreal.log_error("No animations imported; nothing to retarget.")
        for problem in problems:
            unreal.log_error("  " + problem)
        return
    unreal.log("  clips       : {} imported".format(len(sequences)))

    # --- rigs -------------------------------------------------------------
    source_bones = bone_map(source_mesh, problems)
    target_bones = bone_map(target_mesh, problems)

    prefix = find_mixamo_prefix(source_bones)
    if prefix is None:
        unreal.log_error("No 'Hips' bone on {} -- this does not look like a "
                         "Mixamo rig.".format(source_mesh.get_name()))
        return
    unreal.log("  bone prefix : '{}' ({} source bones)".format(
        prefix, len(source_bones)))

    source_pelvis = first_present(source_bones, [prefix + "Hips"])
    target_pelvis = first_present(target_bones, [MANNY_PELVIS])
    target_root = first_present(target_bones, [MANNY_ROOT])

    if target_pelvis is None:
        unreal.log_error("No '{}' bone on {}; --target-mesh does not look like "
                         "a mannequin.".format(MANNY_PELVIS, target_mesh.get_name()))
        return
    if target_root is None:
        problems.append("no '{}' bone on the target: there is nowhere to put "
                        "root motion".format(MANNY_ROOT))

    source_chains = resolve_chains(source_bones, MIXAMO_CHAINS, prefix)
    target_chains = resolve_chains(target_bones, MANNY_CHAINS)

    # A chain present on only one side is not an error -- it just never moves.
    # Keeping the intersection turns that silent failure into a printed line.
    shared = sorted(set(source_chains) & set(target_chains))
    dropped = sorted(set(source_chains) ^ set(target_chains))
    if dropped:
        unreal.log_warning("  chains dropped (one side only): {}".format(
            ", ".join(dropped)))

    source_rig = build_rig(rigs_dir + "/IK_MixamoSource", source_mesh,
                           dict((c, source_chains[c]) for c in shared),
                           source_pelvis, None, "IK_Mixamo", problems)
    target_rig = build_rig(rigs_dir + "/IK_Manny", target_mesh,
                           dict((c, target_chains[c]) for c in shared),
                           target_pelvis, target_root, "IK_Manny", problems)
    if source_rig is None or target_rig is None:
        for problem in problems:
            unreal.log_error("  " + problem)
        return

    # --- retarget ---------------------------------------------------------
    retargeter, rc = build_retargeter(rigs_dir + "/RTG_Mixamo_To_Manny",
                                      source_rig, target_rig, target_root,
                                      target_pelvis, sequences[0],
                                      dict((c, source_chains[c]) for c in shared),
                                      options, switches, problems)

    # auto_map_chains matching by name is the whole retarget, so confirm it took
    # rather than trusting that identical keys implies a mapping.
    mapped = []
    for chain in shared:
        try:
            if str(rc.get_source_chain(chain)) not in ("None", ""):
                mapped.append(chain)
        except Exception:
            pass
    unreal.log("  chains      : {}/{} mapped".format(len(mapped), len(shared)))
    unmapped = [c for c in shared if c not in mapped]
    if unmapped:
        problems.append("chains present on both rigs but not mapped: {}".format(
            ", ".join(unmapped)))

    produced = batch_retarget(retargeter, sequences, clips_dir, source_mesh,
                              target_mesh, options, problems)

    if not switches["--no-root-motion"]:
        for sequence in produced:
            apply_root_motion_settings(sequence)

    unreal.EditorAssetLibrary.save_directory(work, only_if_is_dirty=False)
    unreal.EditorAssetLibrary.save_directory(options["--dest"], only_if_is_dirty=False)

    # --- report -----------------------------------------------------------
    unreal.log("-" * 74)
    unreal.log("  {} sequence(s) on {}".format(
        len(produced), target_mesh.get_editor_property("skeleton").get_name()))
    unreal.log("      {:<38} {:>7} {:>6} {:>10}".format(
        "name", "length", "frames", "root move"))

    still = []
    for sequence in sorted(produced, key=lambda s: s.get_name()):
        travel = root_travel(sequence, target_root) if target_root else -1.0
        unreal.log("      {:<38} {:>6.2f}s {:>5}f {:>9}".format(
            sequence.get_name(),
            unreal.AnimationLibrary.get_sequence_length(sequence),
            unreal.AnimationLibrary.get_num_frames(sequence),
            "n/a" if travel < 0 else "{:.1f}uu".format(travel)))
        if 0 <= travel < 1.0:
            still.append(sequence.get_name())

    if still and not switches["--no-root-motion"]:
        unreal.log("  in place (root never moves), which is expected for a clip")
        unreal.log("  downloaded from Mixamo with 'In Place' ticked:")
        for name in still:
            unreal.log("      {}".format(name))

    if problems:
        unreal.log_warning("  PROBLEMS:")
        for problem in problems:
            unreal.log_warning("      {}".format(problem))
    unreal.log("=" * 74)


run()
