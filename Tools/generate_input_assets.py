"""Creates the Input Action assets and mapping context the modal scheme needs.

Run from anywhere:

    "<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" <path>/arpg.uproject \
        -ExecutePythonScript="<path>/Tools/generate_input_assets.py" \
        -unattended -nopause -nosplash

RE-RUNNABLE. Existing actions are reused rather than replaced, so running it
again after hand-editing one keeps your edits. Only the mapping context's own
mapping list is rebuilt, because a list of mappings is the one thing here that
cannot be merged sensibly -- and it is the part that is derived rather than
authored.

WHY A SCRIPT AND NOT HAND-AUTHORING. Thirteen actions with two keys each is a
lot of clicking to get exactly right, and the layout is not arbitrary: it
mirrors the scheme documented on UARPGModalInputComponent, where a typo produces
a control that silently does nothing rather than an error. Generating it from a
table that sits next to that documentation keeps the two honest.

THIS CONTEXT DELIBERATELY DOES NOT CONTAIN MOVE OR LOOK. Those stay in the
template's IMC_Default, which the player controller already applies; both
contexts are active at once. Duplicating them here would mean two sources of
truth for the stick layout.
"""

import unreal

CONTENT_DIR = "/Game/ARPG/Input"
IMC_NAME = "IMC_ARPG"

# name -> (gamepad key, keyboard fallback)
#
# The keyboard column exists so the scheme can be exercised without a pad. It
# reads oddly by nature -- this is a controller layout, and a modifier held on a
# trigger does not have a natural keyboard equivalent.
#
# Face buttons follow the modal component's slot order, North/West/South/East,
# which is also the element loadout's slot order. That correspondence is the
# whole reason LT + a face button can mean "ready THAT element".
ACTIONS = [
    ("IA_ARPG_FaceNorth",         "Gamepad_FaceButton_Top",    "F"),
    ("IA_ARPG_FaceWest",          "Gamepad_FaceButton_Left",   "R"),
    ("IA_ARPG_FaceSouth",         "Gamepad_FaceButton_Bottom", "SpaceBar"),
    ("IA_ARPG_FaceEast",          "Gamepad_FaceButton_Right",  "LeftShift"),
    ("IA_ARPG_MagicModifier",     "Gamepad_LeftTrigger",       "Q"),
    ("IA_ARPG_DischargeModifier", "Gamepad_RightTrigger",      "E"),
    ("IA_ARPG_SpecialAttack",     "Gamepad_RightShoulder",     "RightMouseButton"),
    ("IA_ARPG_Parry",             "Gamepad_LeftShoulder",      "MiddleMouseButton"),
    ("IA_ARPG_DPadUp",            "Gamepad_DPad_Up",           "One"),
    ("IA_ARPG_DPadDown",          "Gamepad_DPad_Down",         "Two"),
    ("IA_ARPG_DPadLeft",          "Gamepad_DPad_Left",         "Three"),
    ("IA_ARPG_DPadRight",         "Gamepad_DPad_Right",        "Four"),
    ("IA_ARPG_Sprint",            "Gamepad_LeftThumbstick",    "LeftAlt"),
]

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def log(message):
    unreal.log("[ARPG input] {}".format(message))


def save(asset, path):
    """Saves, and treats a refusal as an error rather than a silent no-op.

    THE RETURN VALUE MATTERS. A package whose file is held open by another
    process -- an editor with the asset loaded is enough -- fails to save with
    only a warning, and the object in memory still holds everything you just
    wrote. Verifying by reading the loaded object back therefore reports success
    against a file that never changed, which is exactly how a broken asset
    survived several "verified" runs of this script.
    """
    if unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
        return True

    log("SAVE REFUSED for {} -- is the editor open with this asset loaded?".format(path))
    return False


def make_key(key_name):
    """FKey has no Python constructor; its text form is the way in."""
    key = unreal.Key()
    key.import_text(key_name)
    return key


def ensure_action(name):
    path = "{}/{}".format(CONTENT_DIR, name)

    if unreal.EditorAssetLibrary.does_asset_exist(path):
        return unreal.EditorAssetLibrary.load_asset(path)

    action = asset_tools.create_asset(
        asset_name=name,
        package_path=CONTENT_DIR,
        asset_class=unreal.InputAction,
        factory=unreal.InputAction_Factory(),
    )

    # Boolean: every control in the modal scheme is a press or a hold. The two
    # analogue actions the game needs -- move and look -- are the template's and
    # are deliberately left where they are.
    action.set_editor_property("value_type", unreal.InputActionValueType.BOOLEAN)
    save(action, path)
    log("created {}".format(path))
    return action


def main():
    unreal.EditorAssetLibrary.make_directory(CONTENT_DIR)

    imc_path = "{}/{}".format(CONTENT_DIR, IMC_NAME)

    # EMPTIED AND REFILLED rather than deleted and recreated: a package deleted
    # in this session still holds its name, so the recreate fails and you are
    # left with nothing at all.
    #
    # The mapping list is derived rather than authored, so wiping it is correct
    # -- an asset carrying entries from a previous layout is worse than no asset,
    # because the stale ones still bind.
    if unreal.EditorAssetLibrary.does_asset_exist(imc_path):
        imc = unreal.EditorAssetLibrary.load_asset(imc_path)
        imc.unmap_all()
    else:
        imc = asset_tools.create_asset(
            asset_name=IMC_NAME,
            package_path=CONTENT_DIR,
            asset_class=unreal.InputMappingContext,
            factory=unreal.InputMappingContext_Factory(),
        )
        log("created {}".format(imc_path))

    if not imc:
        log("could not open or create {}".format(imc_path))
        return

    expected = 0
    for name, gamepad_key, keyboard_key in ACTIONS:
        action = ensure_action(name)
        if not action:
            log("FAILED to create {} -- skipping".format(name))
            continue

        for key_name in (gamepad_key, keyboard_key):
            if not key_name:
                continue

            # MapKey, NOT set_editor_property on the mappings array. There are
            # two arrays on this asset: a deprecated `Mappings` and the
            # `DefaultKeyMappings` struct the engine actually reads. Writing the
            # first produces an asset that looks right, reads back exactly what
            # you wrote, and binds nothing -- and PostLoad only migrates it for
            # assets saved before the change, which a freshly written one is
            # not. MapKey puts it in the right place by construction.
            imc.map_key(action, make_key(key_name))
            expected += 1

    # Emptied so nothing is left in the dead property to mislead the next person
    # who opens this asset and sees a full-looking list that binds nothing.
    imc.set_editor_property("mappings", [])

    if not save(imc, imc_path):
        log("IMC_ARPG was NOT written. Close the editor and run this again.")
        return

    # Read back in the property the ENGINE reads, not the one we wrote through.
    written = len(imc.get_editor_property("default_key_mappings")
                  .get_editor_property("mappings"))

    log("{} has {} live mappings (expected {})".format(IMC_NAME, written, expected))
    if written != expected:
        log("MISMATCH -- the mappings did not persist")


main()
