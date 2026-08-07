"""Spot-checks converted AttackDefinition assets against their .tres source."""

import unreal


CHECKS = [
    # asset, property, expected  -- overhead_slash.tres
    ("DA_Attack_OverheadSlash", "attack_id", "sword_overhead_slash"),
    ("DA_Attack_OverheadSlash", "motion_value", 1.6),
    ("DA_Attack_OverheadSlash", "knockback_power", 180.0),
    ("DA_Attack_OverheadSlash", "poise_damage", 12.0),
    ("DA_Attack_OverheadSlash", "finisher_lockout", 0.35),
    ("DA_Attack_OverheadSlash", "stamina_cost", 20.0),
    ("DA_Attack_OverheadSlash", "is_finisher", True),
    ("DA_Attack_OverheadSlash", "blend_locomotion", False),
    ("DA_Attack_OverheadSlash", "movement_speed_factor", 0.2),
    # kick.tres -- the only BODY_FOOT hitbox source in the moveset
    ("DA_Attack_Kick", "hitbox_source", unreal.ARPGHitboxSource.BODY_FOOT),
]


def run():
    failures = []

    for asset_name, prop, expected in CHECKS:
        asset = unreal.EditorAssetLibrary.load_asset(
            "/Game/ARPG/Attacks/{}".format(asset_name))
        if asset is None:
            failures.append("{}: asset missing".format(asset_name))
            continue

        actual = asset.get_editor_property(prop)
        ok = (abs(actual - expected) < 1e-4) if isinstance(expected, float) else (actual == expected)
        if not ok:
            failures.append("{}.{}: expected {!r}, got {!r}".format(
                asset_name, prop, expected, actual))

    # The montage checklist must have survived, or the conversion has silently
    # thrown away the only record of which clips each section needs.
    overhead = unreal.EditorAssetLibrary.load_asset("/Game/ARPG/Attacks/DA_Attack_OverheadSlash")
    import_data = overhead.get_editor_property("import_data")
    for prop, expected in [
        ("source_file", "overhead_slash.tres"),
        ("windup_clip", "overhead_slash_windup"),
        ("active_clip", "overhead_slash_active"),
        ("recovery_clip", "overhead_slash_recovery"),
        ("recovery_cancel_delay", 0.1),
    ]:
        actual = import_data.get_editor_property(prop)
        ok = (abs(actual - expected) < 1e-4) if isinstance(expected, float) else (actual == expected)
        if not ok:
            failures.append("ImportData.{}: expected {!r}, got {!r}".format(prop, expected, actual))

    # Default-valued fields absent from the .tres must keep their C++ defaults
    # rather than being zeroed by the conversion.
    if abs(overhead.get_editor_property("swing_pitch_scale") - 1.0) > 1e-4:
        failures.append("swing_pitch_scale: default was clobbered")

    unreal.log("=" * 68)
    if failures:
        unreal.log_error("VERIFY FAILED ({} problems)".format(len(failures)))
        for entry in failures:
            unreal.log_error("    {}".format(entry))
    else:
        unreal.log("VERIFY PASSED: {} field checks + import data + defaults".format(len(CHECKS)))
    unreal.log("=" * 68)


run()
