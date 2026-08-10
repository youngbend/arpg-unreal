"""Turns on root motion for every animation an ARPG montage plays.

    "<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" <path>/arpg.uproject \
        -ExecutePythonScript="<path>/Tools/enable_montage_root_motion.py" \
        -unattended -nopause -nosplash

CLOSE THE EDITOR FIRST -- a package it holds open cannot be written.

WHY THIS IS NEEDED AT ALL. Root motion is a flag on the ANIMATION SEQUENCE, not
on the montage, and it defaults to off. With it off Unreal extracts no motion
from the root bone, so the bone animates in place: the mesh slides away from the
capsule during the attack and snaps back at the end, while collision, hits and
everything else stay where the character was standing. It looks like a physics
bug and is a single unticked checkbox.

DRIVEN FROM THE MONTAGES rather than from a list of animation names, because the
question "does this animation move the character" is answered by whether an
attack plays it. Author a new montage and re-run this; nothing else to remember.

NOT BLANKET-APPLIED to every animation in the project, which would be wrong:
locomotion is driven by the movement component, and root motion on a run cycle
fights it.

A montage whose animation genuinely should NOT move the character -- a flinch
played in place, say -- can have the flag cleared by hand afterwards. This only
ever turns it on, and reports everything it touched.
"""

import unreal

SEARCH_PATHS = ["/Game/ARPG"]


def log(message):
    unreal.log("[ARPG root motion] {}".format(message))


def save(path):
    """Saves, and treats a refusal as an error rather than a silent no-op."""
    if unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
        return True

    log("SAVE REFUSED for {} -- is the editor open with this asset loaded?".format(path))
    return False


def source_sequences(montage):
    """Every animation the montage plays, across all slots and segments."""
    found = []
    for track in montage.get_editor_property("slot_anim_tracks"):
        anim_track = track.get_editor_property("anim_track")
        for segment in anim_track.get_editor_property("anim_segments"):
            sequence = segment.get_editor_property("anim_reference")
            if sequence and sequence not in found:
                found.append(sequence)
    return found


def main():
    registry = unreal.AssetRegistryHelpers.get_asset_registry()

    montage_class = unreal.TopLevelAssetPath("/Script/Engine", "AnimMontage")
    montages = registry.get_assets_by_class(montage_class, True)

    changed = 0
    seen = 0

    for data in montages:
        path = str(data.package_name)
        if not any(path.startswith(root) for root in SEARCH_PATHS):
            continue

        montage = unreal.EditorAssetLibrary.load_asset(path)
        if not montage:
            continue

        for sequence in source_sequences(montage):
            seen += 1

            if sequence.get_editor_property("enable_root_motion"):
                continue

            sequence.set_editor_property("enable_root_motion", True)

            sequence_path = sequence.get_path_name().split(".")[0]
            if save(sequence_path):
                changed += 1
                log("enabled on {} (played by {})".format(
                    sequence.get_name(), montage.get_name()))

    log("{} of {} montage animations now have root motion".format(
        seen - (seen - changed) if changed else 0, seen))
    log("changed {} this run".format(changed))


main()
