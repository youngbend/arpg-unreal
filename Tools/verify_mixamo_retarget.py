"""Checks that retargeted clips still match their Mixamo sources.

    UnrealEditor-Cmd.exe <project>.uproject ^
        -ExecutePythonScript="<abs>/Tools/verify_mixamo_retarget.py" ^
        -unattended -nullrhi -nosplash

Read-only, so it is safe to run with the editor open.

WHAT IT MEASURES, AND WHY THIS METRIC. For each bone it takes the local rotation
(relative to the parent) and how far that sits from the skeleton's own reference
pose, averaged over the clip. Measuring against each skeleton's own rest pose
makes the number independent of the differing bone axis conventions, so source
and target are directly comparable, and it captures twist -- which the obvious
metric does not.

The obvious metric, the angle between the incoming and outgoing bone directions,
is blind to a bone spinning about its own axis. Ankles scored 4 degrees on it
while visibly corkscrewing.

Comparing against rest rather than against frame 0 matters just as much. A
retarget can track the source's MOTION perfectly while sitting at a permanently
wrong orientation, and a frame-0 baseline subtracts exactly the error you are
looking for. The bug this script was written to catch read as 1.00x against
frame 0 and 2.25x against rest.

WHAT THIS CANNOT TELL YOU. It compares rotation MAGNITUDE, so it cannot tell a
foot rotated 25 degrees forward from one rotated 25 degrees sideways -- both
read 25. A retarget pose that leaves the two characters standing differently
scores clean here. It caught fingers being given 50 degrees of curl they did not
have; it did NOT catch ankles being rotated sideways.

So a pass here means "no joint is being given the wrong AMOUNT of rotation". It
does not mean the retarget is correct. That judgement is made by eye, on the
source-vs-target preview in the retargeter.
"""

import math

import unreal

SOURCE_DIR = "/Game/ARPG/Animations/Mixamo/Clips"
TARGET_DIR = "/Game/ARPG/Animations/Mannequin"
SOURCE_PREFIX = "A_Mixamo_"
TARGET_PREFIX = "A_"

# label -> (mixamo bone, mannequin bone)
BONES = [
    ("L ankle",  "LeftFoot",        "foot_l"),
    ("R ankle",  "RightFoot",       "foot_r"),
    ("L wrist",  "LeftHand",        "hand_l"),
    ("R wrist",  "RightHand",       "hand_r"),
    ("L index1", "LeftHandIndex1",  "index_01_l"),
    ("R index1", "RightHandIndex1", "index_01_r"),
    ("R thumb1", "RightHandThumb1", "thumb_01_r"),
    ("L knee",   "LeftLeg",         "calf_l"),
    ("R elbow",  "RightForeArm",    "lowerarm_r"),
]

SAMPLES = 24
STILL = 0.5  # below this a joint is not really moving, so a ratio is meaningless


def wrap(degrees):
    while degrees > 180.0:
        degrees -= 360.0
    while degrees < -180.0:
        degrees += 360.0
    return degrees


def local_rotator(pose, bone):
    try:
        transform = unreal.AnimPoseExtensions.get_bone_pose(
            pose, bone, unreal.AnimPoseSpaces.LOCAL)
    except Exception:
        return None
    return transform.rotation.rotator()


def deviation(rotators, rest):
    if rest is None or any(r is None for r in rotators):
        return None
    magnitudes = []
    for rotator in rotators:
        roll = wrap(rotator.roll - rest.roll)
        pitch = wrap(rotator.pitch - rest.pitch)
        yaw = wrap(rotator.yaw - rest.yaw)
        magnitudes.append(math.sqrt(roll * roll + pitch * pitch + yaw * yaw))
    return sum(magnitudes) / len(magnitudes) if magnitudes else 0.0


def poses(clip):
    options = unreal.AnimPoseEvaluationOptions()
    length = unreal.AnimationLibrary.get_sequence_length(clip)
    return [unreal.AnimPoseExtensions.get_anim_pose_at_time(
        clip, length * i / float(SAMPLES - 1), options) for i in range(SAMPLES)]


def find_pairs():
    registry = unreal.AssetRegistryHelpers.get_asset_registry()

    def sequences(path):
        return dict(
            (str(d.asset_name), str(d.package_name))
            for d in registry.get_assets_by_path(path, recursive=True)
            if str(d.asset_class_path.asset_name) == "AnimSequence")

    sources = sequences(SOURCE_DIR)
    pairs = []
    for name, package in sorted(sequences(TARGET_DIR).items()):
        if not name.startswith(TARGET_PREFIX):
            continue
        source_name = SOURCE_PREFIX + name[len(TARGET_PREFIX):]
        if source_name in sources:
            pairs.append((sources[source_name], package, name))
    return pairs


def run():
    pairs = find_pairs()
    unreal.log("=" * 78)
    unreal.log("Mixamo retarget fidelity -- mean local rotation from rest pose")
    unreal.log("{:<26} {:>9} {:>9} {:>9} {:>8}".format(
        "clip / bone", "mixamo", "manny", "ratio", "verdict"))

    if not pairs:
        unreal.log_warning("  no source/target pairs found under {} and {}".format(
            SOURCE_DIR, TARGET_DIR))
        return

    failures = []
    for source_package, target_package, name in pairs:
        source = unreal.EditorAssetLibrary.load_asset(source_package)
        target = unreal.EditorAssetLibrary.load_asset(target_package)
        if source is None or target is None:
            continue

        source_poses, target_poses = poses(source), poses(target)
        source_rest = unreal.AnimPoseExtensions.get_reference_pose(
            source.get_editor_property("skeleton"))
        target_rest = unreal.AnimPoseExtensions.get_reference_pose(
            target.get_editor_property("skeleton"))

        unreal.log("-" * 78)
        unreal.log("  {}".format(name))

        for label, source_bone, target_bone in BONES:
            s = deviation([local_rotator(p, source_bone) for p in source_poses],
                          local_rotator(source_rest, source_bone))
            t = deviation([local_rotator(p, target_bone) for p in target_poses],
                          local_rotator(target_rest, target_bone))
            if s is None or t is None:
                unreal.log("    {:<22} {:>9}".format(label, "n/a"))
                continue

            if s < STILL and t < STILL:
                ratio, verdict = None, "still"
            elif s < STILL:
                ratio, verdict = None, "INVENTED"
            else:
                ratio = t / s
                if 0.6 <= ratio <= 1.6:
                    verdict = "ok"
                elif ratio > 2.5 or ratio < 0.35:
                    verdict = "BAD"
                else:
                    verdict = "poor"

            if verdict in ("BAD", "INVENTED", "poor"):
                failures.append("{} {}".format(name, label))

            unreal.log("    {:<22} {:>8.1f}d {:>8.1f}d {:>9} {:>8}".format(
                label, s, t, "-" if ratio is None else "{:.2f}x".format(ratio),
                verdict))

    unreal.log("=" * 78)
    if failures:
        unreal.log_warning("  {} joint(s) off:".format(len(failures)))
        for item in failures:
            unreal.log_warning("      {}".format(item))
    else:
        unreal.log("  no joint is given the wrong AMOUNT of rotation, across "
                   "{} clip(s).".format(len(pairs)))
        unreal.log("  This does NOT mean the retarget is correct -- it cannot "
                   "see a joint rotated")
        unreal.log("  the right amount in the wrong direction. Check the "
                   "retargeter preview by eye.")
    unreal.log("=" * 78)


run()
