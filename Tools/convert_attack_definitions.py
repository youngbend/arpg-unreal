"""
Converts Godot AttackDefinition .tres files into UARPGAttackDefinition assets.

Run from the Unreal editor's Python console, or headless:

    UnrealEditor-Cmd.exe <project>.uproject ^
        -ExecutePythonScript="Tools/convert_attack_definitions.py" ^
        -unattended -nullrhi -nosplash

Source and destination are set by SOURCE_DIR / DEST_PACKAGE_PATH below, or by
the ARPG_TRES_DIR / ARPG_ATTACK_DEST environment variables.

WHAT THIS DOES NOT DO: montages. A .tres names six animation CLIPS that
animation.gd sequenced by hand; the Unreal equivalent is one montage with named
sections, and building that needs the retargeted animation assets. So the
Montage field is left null and the clip names are written into the asset's
ImportData instead, where they read as a checklist for whoever authors it --
including the recovery cancel delay, which becomes the position of the
Event.Attack.ComboWindow notify rather than a runtime field.

Re-running is safe: existing assets are updated in place, so a tuning pass on
the Godot side can be pulled across without losing the montage wiring.
"""

import os
import re

import unreal


# --- Configuration ---------------------------------------------------------

SOURCE_DIR = os.environ.get(
    "ARPG_TRES_DIR",
    r"C:\Users\young\arpg\project\weapons\sword\moveset",
)
DEST_PACKAGE_PATH = os.environ.get("ARPG_ATTACK_DEST", "/Game/ARPG/Attacks")


# --- .tres parsing ---------------------------------------------------------

_RESOURCE_TYPE_RE = re.compile(r'\[gd_resource\s+type="([^"]+)"')
_KEY_VALUE_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$")


def parse_tres(path):
    """Returns (resource_type, {key: parsed_value}) for a flat Godot resource.

    Only the top-level [resource] block is read. These moveset files have no
    sub-resources; if that ever changes this will need extending rather than
    silently dropping them, hence the explicit section tracking.
    """
    resource_type = None
    values = {}
    in_resource = False

    with open(path, "r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith(";"):
                continue

            if line.startswith("["):
                match = _RESOURCE_TYPE_RE.match(line)
                if match:
                    resource_type = match.group(1)
                in_resource = line.startswith("[resource]")
                continue

            if not in_resource:
                continue

            match = _KEY_VALUE_RE.match(line)
            if match:
                values[match.group(1)] = _parse_value(match.group(2).strip())

    return resource_type, values


def _parse_value(text):
    if text.startswith('"') and text.endswith('"'):
        return text[1:-1]
    if text == "true":
        return True
    if text == "false":
        return False
    try:
        return int(text) if re.fullmatch(r"-?\d+", text) else float(text)
    except ValueError:
        # ExtResource(...), SubResource(...) and anything else structured. These
        # moveset files contain none, but returning the raw text means an
        # unexpected one shows up in the report rather than crashing the run.
        return text


# --- Field mapping ---------------------------------------------------------

# Godot key -> Unreal property name. Only fields that survive as runtime data;
# animation names and recovery_cancel_delay go to ImportData instead.
SCALAR_FIELDS = {
    "attack_id": "attack_id",
    "motion_value": "motion_value",
    "motion_value_2": "motion_value_2",
    "landing_motion_value": "landing_motion_value",
    "hitbox_tick_interval": "hitbox_tick_interval",
    "knockback_power": "knockback_power",
    "poise_damage": "poise_damage",
    "hit_stop_duration": "hit_stop_duration",
    "poise_scales_with_motion": "poise_scales_with_motion",
    "hyperarmor": "hyperarmor",
    "unblockable": "unblockable",
    "early_contact": "early_contact",
    "can_be_elemental": "can_be_elemental",
    "is_finisher": "is_finisher",
    "finisher_lockout": "finisher_lockout",
    "continuous_hold": "continuous_hold",
    "chargeable": "chargeable",
    "charge_motion_min": "charge_motion_min",
    "charge_motion_max": "charge_motion_max",
    "charge_time": "charge_time",
    "channel": "channel",
    "channel_stamina_per_second": "channel_stamina_per_second",
    "forward_impulse": "forward_impulse",
    "travel_distance": "travel_distance",
    "travel_charge_scale": "travel_charge_scale",
    "travel_phase": "travel_phase",
    "swing_pitch_scale": "swing_pitch_scale",
    "max_swing_pitch_deg": "max_swing_pitch_deg",
    "stamina_cost": "stamina_cost",
    "blend_locomotion": "blend_locomotion",
    "movement_speed_factor": "movement_speed_factor",
}

CLIP_FIELDS = {
    "windup_animation_name": "windup_clip",
    "active_animation_name": "active_clip",
    "gap_animation_name": "gap_clip",
    "active2_animation_name": "active2_clip",
    "landing_animation_name": "landing_clip",
    "recovery_animation_name": "recovery_clip",
}

# Godot's HitboxSource enum ordinal -> Unreal enum value.
HITBOX_SOURCE = {
    0: unreal.ARPGHitboxSource.WEAPON,
    1: unreal.ARPGHitboxSource.BODY_FOOT,
}

# Keys deliberately not carried across, and why. Anything outside this set and
# the maps above is reported as unmapped rather than dropped quietly.
INTENTIONALLY_SKIPPED = {
    "display_name",          # handled separately, needs FText
    "hitbox_source",         # enum, handled separately
    "recovery_cancel_delay",  # becomes an anim notify position; kept in ImportData
    "damage_type_override",  # an ExtResource path; must be rewired to a UE asset by hand
    "status_effect",         # same
}


def asset_name_for(source_path):
    stem = os.path.splitext(os.path.basename(source_path))[0]
    return "DA_Attack_" + "".join(part.capitalize() for part in stem.split("_"))


def load_or_create(asset_name):
    package = "{}/{}".format(DEST_PACKAGE_PATH, asset_name)

    if unreal.EditorAssetLibrary.does_asset_exist(package):
        return unreal.EditorAssetLibrary.load_asset(package), False

    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.ARPGAttackDefinition)

    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        asset_name, DEST_PACKAGE_PATH, unreal.ARPGAttackDefinition, factory
    )
    return asset, True


def apply_values(asset, values, source_path):
    unmapped = []

    for key, value in values.items():
        if key in SCALAR_FIELDS:
            try:
                asset.set_editor_property(SCALAR_FIELDS[key], value)
            except Exception as error:  # noqa: BLE001 - report, don't abort the batch
                unmapped.append("{} ({})".format(key, error))
        elif key in CLIP_FIELDS or key in INTENTIONALLY_SKIPPED:
            continue
        else:
            unmapped.append(key)

    if "display_name" in values:
        asset.set_editor_property("display_name", unreal.Text(values["display_name"]))

    if "hitbox_source" in values:
        source = HITBOX_SOURCE.get(values["hitbox_source"])
        if source is not None:
            asset.set_editor_property("hitbox_source", source)
        else:
            unmapped.append("hitbox_source={}".format(values["hitbox_source"]))

    # Montage authoring checklist. Rebuilt from scratch each run so a renamed
    # clip on the Godot side does not leave a stale entry behind.
    import_data = unreal.ARPGAttackImportData()
    import_data.set_editor_property("source_file", os.path.basename(source_path))
    for key, prop in CLIP_FIELDS.items():
        import_data.set_editor_property(prop, values.get(key, ""))
    import_data.set_editor_property(
        "recovery_cancel_delay", float(values.get("recovery_cancel_delay", 0.0))
    )
    asset.set_editor_property("import_data", import_data)

    return unmapped


def run():
    if not os.path.isdir(SOURCE_DIR):
        unreal.log_error("Source directory not found: {}".format(SOURCE_DIR))
        return

    created = []
    updated = []
    skipped = []
    problems = []

    for filename in sorted(os.listdir(SOURCE_DIR)):
        if not filename.endswith(".tres"):
            continue

        path = os.path.join(SOURCE_DIR, filename)
        resource_type, values = parse_tres(path)

        # BlockDefinition and FlinchDefinition live alongside the attacks in the
        # same folder; they are different resources with their own ports.
        if resource_type != "AttackDefinition":
            skipped.append("{} ({})".format(filename, resource_type))
            continue

        asset_name = asset_name_for(path)
        asset, was_created = load_or_create(asset_name)
        if asset is None:
            problems.append("{}: could not create asset".format(filename))
            continue

        unmapped = apply_values(asset, values, path)
        if unmapped:
            problems.append("{}: unmapped keys -> {}".format(filename, ", ".join(unmapped)))

        unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False)
        (created if was_created else updated).append(asset_name)

    unreal.log("=" * 68)
    unreal.log("AttackDefinition conversion")
    unreal.log("  source : {}".format(SOURCE_DIR))
    unreal.log("  dest   : {}".format(DEST_PACKAGE_PATH))
    unreal.log("  created: {}".format(len(created)))
    unreal.log("  updated: {}".format(len(updated)))
    for name in created + updated:
        unreal.log("      {}".format(name))
    if skipped:
        unreal.log("  skipped (not AttackDefinition):")
        for entry in skipped:
            unreal.log("      {}".format(entry))
    if problems:
        unreal.log_warning("  needs attention:")
        for entry in problems:
            unreal.log_warning("      {}".format(entry))
    unreal.log("")
    unreal.log("  Montage field is left null by design. Each asset's ImportData")
    unreal.log("  lists the clips its sections need and where the combo cancel")
    unreal.log("  notify goes.")
    unreal.log("=" * 68)


run()
