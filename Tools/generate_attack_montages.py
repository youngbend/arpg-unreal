"""
Creates one montage per attack, seeded with its Windup clip, and wires each
UARPGAttackDefinition to it. Then prints exactly what remains to be done by hand.

    UnrealEditor-Cmd.exe <project>.uproject ^
        -ExecutePythonScript="Tools/generate_attack_montages.py" ^
        -unattended -nullrhi -nosplash

WHY THIS IS ONLY PARTLY AUTOMATED. Montages can be created from script with a
single segment -- AnimMontageFactory.source_animation does that correctly,
including length and slot. Two things cannot:

  Extra segments. Writing SlotAnimTracks[0].AnimTrack.AnimSegments through
  set_editor_property does not persist; the montage keeps the one segment the
  factory gave it. Verified by reading the array straight back.

  Named sections. UAnimMontage.CompositeSections is not exposed to Python at
  all, so Windup/Active/Recovery cannot be created programmatically.

Sections are not optional -- charge loops the Windup section and channel loops
the Active section -- so the honest answer is to automate the setup and hand over
a precise checklist, rather than reshape the design around a tooling gap.
"""

import unreal


ATTACKS_PATH = "/Game/ARPG/Attacks"
ANIM_PATH = "/Game/ARPG/Animations/Mannequin"
MONTAGE_PATH = "/Game/ARPG/Animations/Montages"

# Longest first: cross_slash_combo must win over cross_slash.
FILE_STEMS = [
    "cross_slash_combo", "one_hand_combo", "two_hand_combo",
    "high_spinning_slash", "low_spinning_slash",
    "downward_slash", "jumping_slash", "overhead_slash", "cross_slash", "kick",
]

PHASE_ORDER = ["windup_clip", "active_clip", "gap_clip", "active2_clip",
               "landing_clip", "recovery_clip"]
PHASE_SECTION = {
    "windup_clip": "Windup",
    "active_clip": "Active",
    "gap_clip": "Gap",
    "active2_clip": "Active2",
    "landing_clip": "Landing",
    "recovery_clip": "Recovery",
}


def title_case(text):
    return "".join(p.capitalize() for p in text.replace("-", "_").split("_") if p)


def title_parts(text):
    return "_".join(p.capitalize() for p in text.replace("-", "_").split("_") if p)


def clip_to_asset_name(godot_clip):
    """overhead_slash_windup -> A_Manny_OverheadSlash_Windup

    Godot named clips <file_stem>_<anim_name>, and the import named the assets
    A_Manny_<TitleStem>_<TitleAnim>, so splitting on the known stems reconstructs
    the asset name without needing a hand-written table.
    """
    for stem in FILE_STEMS:
        if godot_clip.startswith(stem):
            remainder = godot_clip[len(stem):].lstrip("_")
            if remainder:
                return "A_Manny_{}_{}".format(title_case(stem), title_parts(remainder))
    return None


def run():
    reg = unreal.AssetRegistryHelpers.get_asset_registry()

    attacks = []
    for data in reg.get_assets_by_path(ATTACKS_PATH, recursive=True):
        asset = unreal.EditorAssetLibrary.load_asset(str(data.package_name))
        if isinstance(asset, unreal.ARPGAttackDefinition):
            attacks.append(asset)

    unreal.log("=" * 78)
    unreal.log("Attack montages")
    unreal.log("  attacks: {}".format(len(attacks)))

    created = 0
    problems = []
    checklist = []

    for attack in sorted(attacks, key=lambda a: a.get_name()):
        import_data = attack.get_editor_property("import_data")

        phases = []
        for prop in PHASE_ORDER:
            clip = str(import_data.get_editor_property(prop) or "")
            if not clip:
                continue
            asset_name = clip_to_asset_name(clip)
            if asset_name is None:
                problems.append("{}: cannot map clip '{}'".format(attack.get_name(), clip))
                continue
            seq = unreal.EditorAssetLibrary.load_asset("{}/{}".format(ANIM_PATH, asset_name))
            if seq is None:
                problems.append("{}: missing animation {}".format(attack.get_name(), asset_name))
                continue
            phases.append((PHASE_SECTION[prop], seq))

        if not phases:
            problems.append("{}: no clips resolved".format(attack.get_name()))
            continue

        montage_name = "AM_" + attack.get_name().replace("DA_Attack_", "")
        package = "{}/{}".format(MONTAGE_PATH, montage_name)

        if not unreal.EditorAssetLibrary.does_asset_exist(package):
            factory = unreal.AnimMontageFactory()
            factory.set_editor_property("target_skeleton", phases[0][1].get_editor_property("skeleton"))
            # Seeds a real, correctly-formed single-segment montage.
            factory.set_editor_property("source_animation", phases[0][1])
            montage = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
                montage_name, MONTAGE_PATH, unreal.AnimMontage, factory)
            created += 1
        else:
            montage = unreal.EditorAssetLibrary.load_asset(package)

        if montage is None:
            problems.append("{}: could not create montage".format(attack.get_name()))
            continue

        attack.set_editor_property("montage", montage)

        # Build the finishing instructions while the durations are to hand.
        slot = "UpperBody" if attack.get_editor_property("blend_locomotion") else "FullBody"
        cancel = import_data.get_editor_property("recovery_cancel_delay")

        lines = ["{}  (slot: {})".format(montage_name, slot)]
        elapsed = 0.0
        for index, (section, seq) in enumerate(phases):
            length = unreal.AnimationLibrary.get_sequence_length(seq)
            marker = "seeded" if index == 0 else "ADD"
            lines.append("      {:<6} {:<10} @ {:>5.2f}s  {:<34} ({:.2f}s)".format(
                marker, section, elapsed, seq.get_name(), length))
            elapsed += length

        # Hitbox window spans the active sections; the combo notify sits inside
        # recovery at the delay the Godot data authored.
        active_start = None
        active_end = None
        t = 0.0
        recovery_start = None
        for section, seq in phases:
            length = unreal.AnimationLibrary.get_sequence_length(seq)
            if section == "Active" and active_start is None:
                active_start = t
            if section in ("Active", "Active2"):
                active_end = t + length
            if section == "Recovery":
                recovery_start = t
            t += length

        if active_start is not None:
            lines.append("      notify  Hitbox window  {:.2f}s -> {:.2f}s".format(
                active_start, active_end))
        if recovery_start is not None:
            lines.append("      notify  ComboWindow    {:.2f}s  (recovery + {:.2f})".format(
                recovery_start + cancel, cancel))

        checklist.append(lines)

    unreal.EditorAssetLibrary.save_directory("/Game/ARPG", only_if_is_dirty=False)

    unreal.log("  montages created: {}".format(created))
    unreal.log("")
    unreal.log("  TO FINISH EACH MONTAGE (segments and sections are not scriptable):")
    unreal.log("    1. open it, drag the ADD clips onto the same slot track, in order")
    unreal.log("    2. create a section at each clip boundary, named as shown")
    unreal.log("    3. place the two notifies at the times given")
    unreal.log("")
    # One unreal.log call per line: a multi-line string only emits its first line.
    for lines in checklist:
        for line in lines:
            unreal.log("  " + line)

    if problems:
        unreal.log_warning("  PROBLEMS:")
        for p in problems:
            unreal.log_warning("      {}".format(p))
    unreal.log("=" * 78)


run()
