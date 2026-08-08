"""
Checks the retargeted animations against their sources.

Catches the failure this pipeline is most prone to: chains that did not map, or
a bad pose alignment, produce clips that exist, have the right length, and
animate nothing -- a frozen T-pose. Comparing durations alone would pass.
"""

import unreal

SOURCE_DIR = "/Game/ARPG/Animations/Paladin/Attacks"
TARGET_DIR = "/Game/ARPG/Animations/Mannequin"

# Bones sampled for actual movement. If a swing does not rotate the sword arm,
# the retarget did not work regardless of what else looks right.
SAMPLE_BONES = ["upperarm_r", "lowerarm_r", "hand_r", "spine_03", "thigh_l"]


def sequences_in(path):
    reg = unreal.AssetRegistryHelpers.get_asset_registry()
    out = {}
    for data in reg.get_assets_by_path(path, recursive=True):
        if str(data.asset_class_path.asset_name) != "AnimSequence":
            continue
        asset = unreal.EditorAssetLibrary.load_asset(str(data.package_name))
        if asset:
            out[asset.get_name()] = asset
    return out


def rotation_spread(seq, bone):
    """Largest angle (degrees) any sampled frame differs from the first."""
    length = unreal.AnimationLibrary.get_sequence_length(seq)
    if length <= 0.0:
        return 0.0

    try:
        base = unreal.AnimationLibrary.get_bone_pose_for_time(seq, bone, 0.0, False).rotation
    except Exception:
        return -1.0  # bone not on this skeleton

    worst = 0.0
    steps = 8
    for i in range(1, steps + 1):
        t = length * (float(i) / steps)
        try:
            rot = unreal.AnimationLibrary.get_bone_pose_for_time(seq, bone, t, False).rotation
        except Exception:
            continue
        # Angle between two quaternions.
        dot = abs(base.x * rot.x + base.y * rot.y + base.z * rot.z + base.w * rot.w)
        dot = min(1.0, max(-1.0, dot))
        import math
        worst = max(worst, math.degrees(2.0 * math.acos(dot)))
    return worst


def run():
    sources = sequences_in(SOURCE_DIR)
    targets = sequences_in(TARGET_DIR)

    unreal.log("=" * 78)
    unreal.log("Retarget verification")
    unreal.log("  source clips : {}".format(len(sources)))
    unreal.log("  target clips : {}".format(len(targets)))

    problems = []
    frozen = []
    checked = 0

    for name, target in sorted(targets.items()):
        source_name = name.replace("A_Manny_", "A_", 1)
        source = sources.get(source_name)

        if source is None:
            problems.append("{}: no matching source ({})".format(name, source_name))
            continue

        src_frames = unreal.AnimationLibrary.get_num_frames(source)
        tgt_frames = unreal.AnimationLibrary.get_num_frames(target)
        src_len = unreal.AnimationLibrary.get_sequence_length(source)
        tgt_len = unreal.AnimationLibrary.get_sequence_length(target)

        if src_frames != tgt_frames:
            problems.append("{}: frames {} -> {}".format(name, src_frames, tgt_frames))
        if abs(src_len - tgt_len) > 0.01:
            problems.append("{}: length {:.2f} -> {:.2f}".format(name, src_len, tgt_len))

        skeleton = target.get_editor_property("skeleton")
        if skeleton is None or "Mannequin" not in skeleton.get_name():
            problems.append("{}: unexpected skeleton {}".format(
                name, skeleton.get_name() if skeleton else "None"))

        # Does it actually move?
        spreads = {b: rotation_spread(target, b) for b in SAMPLE_BONES}
        moving = [b for b, s in spreads.items() if s > 2.0]
        if not moving:
            frozen.append((name, spreads))
        checked += 1

    unreal.log("  compared     : {}".format(checked))

    if frozen:
        unreal.log_error("  FROZEN -- no sampled bone rotates (retarget did not apply):")
        for name, spreads in frozen[:10]:
            unreal.log_error("      {}  {}".format(
                name, {b: round(s, 1) for b, s in spreads.items()}))
    else:
        unreal.log("  all clips animate (sampled bones rotate)")

    # Show a few real numbers so 'it moved' is visible, not just asserted.
    unreal.log("  sample motion (max degrees from frame 0):")
    for name, target in sorted(targets.items())[:5]:
        spreads = {b: round(rotation_spread(target, b), 1) for b in SAMPLE_BONES}
        unreal.log("      {:<42} {}".format(name, spreads))

    if problems:
        unreal.log_warning("  PROBLEMS ({}):".format(len(problems)))
        for p in problems[:15]:
            unreal.log_warning("      {}".format(p))
    else:
        unreal.log("  timing and skeleton checks passed")
    unreal.log("=" * 78)


run()
