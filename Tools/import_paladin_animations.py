"""
Imports the paladin rig and its attack animations.

    UnrealEditor-Cmd.exe <project>.uproject ^
        -ExecutePythonScript="Tools/import_paladin_animations.py" ^
        -unattended -nullrhi -nosplash

Two sources, and they are NOT the same skeleton naming:

    kimodo_retarget/paladin.glb          mesh + skin, bones named mixamorig_Hips
    edited_animations_v3/*.glb           animation only, bones named mixamorig:Hips

Unreal sanitises ':' out of bone names on import, so both should land as
mixamorig_Hips and the animations should bind to the rig's skeleton. That is an
assumption worth checking rather than trusting -- this script reports the actual
bone names and flags any animation that imported with no tracks, which is what a
naming mismatch looks like.

The clips are authored in metres (hips sit at Y=0.96); Unreal works in
centimetres. Scale is deliberately NOT forced here -- see the report at the end.
"""

import os

import unreal


RIG_SOURCE = r"C:\Users\young\arpg\kimodo_retarget\paladin.glb"
ANIM_SOURCE_DIR = r"C:\Users\young\Downloads\edited_animations_v3"

RIG_DEST = "/Game/ARPG/Animations/Paladin"
ANIM_DEST = "/Game/ARPG/Animations/Paladin/Attacks"


def import_tasks(tasks):
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
    created = []
    for task in tasks:
        created.extend(task.get_editor_property("imported_object_paths") or [])
    return created


def make_task(filename, destination, options=None):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", filename)
    task.set_editor_property("destination_path", destination)
    task.set_editor_property("automated", True)
    task.set_editor_property("save", True)
    task.set_editor_property("replace_existing", True)
    if options is not None:
        task.set_editor_property("options", options)
    return task


def find_asset_of_class(paths, cls):
    for path in paths:
        asset = unreal.EditorAssetLibrary.load_asset(path.split(".")[0])
        if isinstance(asset, cls):
            return asset
    return None


def animation_pipeline_for(skeleton):
    """A pipeline that imports animation only, bound to an existing skeleton."""
    pipeline = unreal.InterchangeGenericAssetsPipeline()

    common = pipeline.get_editor_property("common_skeletal_meshes_and_animations_properties")
    common.set_editor_property("skeleton", skeleton)
    # Without this the importer happily creates a second skeletal mesh and a
    # second skeleton per file, and the animations bind to the wrong one.
    common.set_editor_property("import_only_animations", True)

    mesh_pipeline = pipeline.get_editor_property("mesh_pipeline")
    mesh_pipeline.set_editor_property("import_skeletal_meshes", False)
    mesh_pipeline.set_editor_property("import_static_meshes", False)

    return pipeline


def run():
    if not os.path.isfile(RIG_SOURCE):
        unreal.log_error("Rig not found: {}".format(RIG_SOURCE))
        return
    if not os.path.isdir(ANIM_SOURCE_DIR):
        unreal.log_error("Animation directory not found: {}".format(ANIM_SOURCE_DIR))
        return

    # --- 1. The rig -------------------------------------------------------
    rig_paths = import_tasks([make_task(RIG_SOURCE, RIG_DEST)])
    skeletal_mesh = find_asset_of_class(rig_paths, unreal.SkeletalMesh)

    if skeletal_mesh is None:
        unreal.log_error("No skeletal mesh came out of paladin.glb. Imported: {}".format(rig_paths))
        return

    skeleton = skeletal_mesh.get_editor_property("skeleton")

    # --- 2. The animations ------------------------------------------------
    pipeline = animation_pipeline_for(skeleton)

    anim_tasks = []
    for filename in sorted(os.listdir(ANIM_SOURCE_DIR)):
        if filename.lower().endswith(".glb"):
            anim_tasks.append(
                make_task(os.path.join(ANIM_SOURCE_DIR, filename), ANIM_DEST, pipeline))

    anim_paths = import_tasks(anim_tasks)

    sequences = []
    for path in anim_paths:
        asset = unreal.EditorAssetLibrary.load_asset(path.split(".")[0])
        if isinstance(asset, unreal.AnimSequence):
            sequences.append(asset)

    # --- 3. Report --------------------------------------------------------
    unreal.log("=" * 70)
    unreal.log("Paladin import")
    unreal.log("  skeletal mesh : {}".format(skeletal_mesh.get_path_name()))
    unreal.log("  skeleton      : {}".format(skeleton.get_path_name()))

    bounds = skeletal_mesh.get_bounds()
    extent = bounds.box_extent
    unreal.log("  mesh height   : {:.2f} uu  (mannequin is ~180; ~1.8 means the".format(extent.z * 2.0))
    unreal.log("                  clips are in metres and need a 100x scale)")

    unreal.log("  source files  : {}".format(len(anim_tasks)))
    unreal.log("  sequences     : {}".format(len(sequences)))

    empty = []
    for seq in sorted(sequences, key=lambda s: s.get_name()):
        length = unreal.AnimationLibrary.get_sequence_length(seq)
        frames = unreal.AnimationLibrary.get_num_frames(seq)
        tracks = unreal.AnimationLibrary.get_animation_track_names(seq)
        unreal.log("      {:<40} {:>6.2f}s {:>4}f {:>4} tracks".format(
            seq.get_name(), length, frames, len(tracks)))
        # The direct symptom of a bone-name mismatch: the sequence imports, but
        # binds to nothing and animates nothing.
        if frames <= 1 or len(tracks) == 0:
            empty.append(seq.get_name())

    # Track names are the ground truth on the ':' vs '_' question -- if these
    # read mixamorig_Hips then Unreal sanitised both sides identically and the
    # animations really are bound to the rig.
    if sequences:
        sample = unreal.AnimationLibrary.get_animation_track_names(sequences[0])
        unreal.log("  bound bone names (first 6): {}".format([str(n) for n in sample[:6]]))

    if empty:
        unreal.log_warning("  SUSPECT -- imported but animates nothing (bone-name mismatch?):")
        for name in empty:
            unreal.log_warning("      {}".format(name))
    unreal.log("=" * 70)


run()
