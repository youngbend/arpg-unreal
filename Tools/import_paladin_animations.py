"""
Imports the paladin rig and its attack animations.

    UnrealEditor-Cmd.exe <project>.uproject ^
        -ExecutePythonScript="Tools/import_paladin_animations.py" ^
        -unattended -nullrhi -nosplash

Two sources, and they disagree on bone naming:

    kimodo_retarget/paladin.glb     mesh + skin, bones named mixamorig_Hips
    edited_animations_v3/*.glb      animation only, bones named mixamorig:Hips

Unreal sanitises ':' out of bone names, so both land as mixamorig_Hips. This
verifies that rather than assuming it, by reporting the bound track names and
flagging any sequence that imports with no tracks -- which is exactly what a
naming mismatch looks like.

WHY THE ANIMATIONS NEED A CONFIGURED PIPELINE. The attack GLBs contain a skin and
animation but NO MESH, so a plain import produces nothing at all: the translator
has no mesh to build a skeletal mesh from, and without a target skeleton it has
nothing to bind animation to either. They have to be imported animation-only
against the rig's skeleton, and the pipeline carrying that has to be passed
through ImportAssetParameters.override_pipelines -- AssetImportTask.options is
silently ignored for Interchange, which fails as a silent no-op rather than an
error.

Scale needs no correction: the glTF importer already converts metres to
centimetres, and the paladin lands at ~172uu against the mannequin's ~180.
"""

import os

import unreal


RIG_SOURCE = r"C:\Users\young\arpg\kimodo_retarget\paladin.glb"
ANIM_SOURCE_DIR = r"C:\Users\young\Downloads\edited_animations_v3"

RIG_DEST = "/Game/ARPG/Animations/Paladin/Rig"
ANIM_DEST = "/Game/ARPG/Animations/Paladin/Attacks"


def import_rig():
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", RIG_SOURCE)
    task.set_editor_property("destination_path", RIG_DEST)
    task.set_editor_property("automated", True)
    task.set_editor_property("save", True)
    task.set_editor_property("replace_existing", True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    for path in task.get_editor_property("imported_object_paths") or []:
        asset = unreal.EditorAssetLibrary.load_asset(path.split(".")[0])
        # paladin.glb has two skins (body and helmet); the body carries the
        # skeleton we want to retarget from.
        if isinstance(asset, unreal.SkeletalMesh) and "helmet" not in asset.get_name().lower():
            return asset
    return None


def animation_pipeline(skeleton):
    pipeline = unreal.InterchangeGenericAssetsPipeline()

    common = pipeline.get_editor_property("common_skeletal_meshes_and_animations_properties")
    common.set_editor_property("skeleton", skeleton)
    common.set_editor_property("import_only_animations", True)

    anim = pipeline.get_editor_property("animation_pipeline")
    anim.set_editor_property("import_animations", True)
    anim.set_editor_property("import_bone_tracks", True)

    mesh = pipeline.get_editor_property("mesh_pipeline")
    mesh.set_editor_property("import_skeletal_meshes", False)
    mesh.set_editor_property("import_static_meshes", False)

    return pipeline


def title_case(text):
    """one_hand_combo -> OneHandCombo"""
    return "".join(part.capitalize() for part in text.replace("-", "_").split("_") if part)


def title_parts(text):
    """attack_1_2_active -> Attack_1_2_Active, keeping the separators readable."""
    return "_".join(part.capitalize() for part in text.replace("-", "_").split("_") if part)


def strip_prefix(name, stem):
    """Removes the source file's name from the front of an imported asset name.

    Unreal concatenates <filestem><animname> with no separator, and drops the
    underscores from the stem in some cases but not others -- 'kick' + 'active'
    becomes 'kickactive', while 'cross_slash' + 'active' becomes
    'cross_slashactive'. Both spellings have to be tried or the prefix survives
    into the final name twice.
    """
    lowered = name.lower()
    for candidate in (stem.lower(), stem.replace("_", "").lower()):
        if lowered.startswith(candidate):
            return name[len(candidate):].lstrip("_")
    return name


def import_animations(skeleton):
    manager = unreal.InterchangeManager.get_interchange_manager_scripted()
    sequences = []

    for filename in sorted(os.listdir(ANIM_SOURCE_DIR)):
        if not filename.lower().endswith(".glb"):
            continue

        stem = os.path.splitext(filename)[0]
        destination = "{}/{}".format(ANIM_DEST, title_case(stem))

        params = unreal.ImportAssetParameters()
        params.set_editor_property("is_automated", True)
        params.set_editor_property("replace_existing", True)
        # The pipeline object is transient; its path resolves for the duration
        # of this call, which is all the importer needs.
        params.set_editor_property(
            "override_pipelines",
            [unreal.SoftObjectPath(animation_pipeline(skeleton).get_path_name())])

        source = unreal.InterchangeManager.create_source_data(
            os.path.join(ANIM_SOURCE_DIR, filename))
        manager.import_asset(destination, source, params)

        # Unreal names these <filestem><animname> with no separator. Rename to a
        # predictable A_<Attack>_<Phase> so 42 assets stay navigable and the
        # montage checklist in each DA_Attack_* is easy to match up.
        for asset_path in unreal.EditorAssetLibrary.list_assets(destination, recursive=False) or []:
            asset = unreal.EditorAssetLibrary.load_asset(asset_path.split(".")[0])
            if not isinstance(asset, unreal.AnimSequence):
                continue

            current = asset.get_name()
            suffix = strip_prefix(current, stem)
            desired = "A_{}_{}".format(title_case(stem), title_parts(suffix) or "Anim")

            if current != desired:
                new_path = "{}/{}".format(destination, desired)
                if not unreal.EditorAssetLibrary.does_asset_exist(new_path):
                    unreal.EditorAssetLibrary.rename_asset(
                        asset_path.split(".")[0], new_path)
                    asset = unreal.EditorAssetLibrary.load_asset(new_path)

            sequences.append(asset)

    return sequences


def run():
    if not os.path.isfile(RIG_SOURCE):
        unreal.log_error("Rig not found: {}".format(RIG_SOURCE))
        return
    if not os.path.isdir(ANIM_SOURCE_DIR):
        unreal.log_error("Animation directory not found: {}".format(ANIM_SOURCE_DIR))
        return

    skeletal_mesh = import_rig()
    if skeletal_mesh is None:
        unreal.log_error("No skeletal mesh came out of paladin.glb.")
        return

    skeleton = skeletal_mesh.get_editor_property("skeleton")
    sequences = import_animations(skeleton)

    unreal.EditorAssetLibrary.save_directory("/Game/ARPG/Animations", only_if_is_dirty=False)

    # --- Report -----------------------------------------------------------
    unreal.log("=" * 74)
    unreal.log("Paladin import")
    unreal.log("  skeletal mesh : {}".format(skeletal_mesh.get_path_name()))
    unreal.log("  skeleton      : {}".format(skeleton.get_path_name()))

    extent = skeletal_mesh.get_bounds().box_extent
    unreal.log("  mesh height   : {:.1f} uu (mannequin ~180)".format(extent.z * 2.0))
    unreal.log("  sequences     : {}".format(len(sequences)))

    suspect = []
    for seq in sorted(sequences, key=lambda s: s.get_name()):
        frames = unreal.AnimationLibrary.get_num_frames(seq)
        length = unreal.AnimationLibrary.get_sequence_length(seq)
        tracks = unreal.AnimationLibrary.get_animation_track_names(seq)
        unreal.log("      {:<38} {:>6.2f}s {:>4}f {:>4} tracks".format(
            seq.get_name(), length, frames, len(tracks)))
        if frames <= 1 or len(tracks) == 0:
            suspect.append(seq.get_name())

    if sequences:
        sample = unreal.AnimationLibrary.get_animation_track_names(sequences[0])
        unreal.log("  bound bones   : {}".format([str(n) for n in sample[:5]]))

    if suspect:
        unreal.log_warning("  SUSPECT -- imported but animates nothing:")
        for name in suspect:
            unreal.log_warning("      {}".format(name))
    else:
        unreal.log("  all sequences carry bone tracks")
    unreal.log("=" * 74)


run()
