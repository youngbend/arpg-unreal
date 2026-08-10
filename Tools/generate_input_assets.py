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

THE XINPUT AND RAWINPUT KEYS BOTH GO IN, side by side. An Xbox-compatible pad
speaks Gamepad_*; a DualSense read through the RawInput plugin speaks
GenericUSBController_* instead, and no single device produces both. Mapping
both means either pad works with no profile to pick and nothing to switch.

MOVE AND LOOK APPEAR HERE FOR THE RAW PAD ONLY. Their XInput and keyboard
mappings stay in the template's IMC_Default, which the player controller
applies alongside this one; what is added here is the raw device's sticks,
which IMC_Default has no way to know about. That is a device gaining a voice,
not a second stick layout.
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
# name -> (XInput key, keyboard fallback, RawInput key)
#
# The RawInput column is the DualSense's HID button numbering, passed straight
# through in Config/DefaultInput.ini as button N -> ButtonN so this table and
# that file describe the same thing. Physically:
#
#   Button1 Square   Button2 Cross   Button3 Circle   Button4 Triangle
#   Button5 L1       Button6 R1      Button7 L2       Button8 R2
#   Button11 L3
#
# Which lands each face where an Xbox pad puts its counterpart: Triangle where
# Y is, Square where X is. The scheme needs no per-device layout as a result.
ACTIONS = [
    ("IA_ARPG_FaceNorth",         "Gamepad_FaceButton_Top",    "F",
     "GenericUSBController_Button4"),
    ("IA_ARPG_FaceWest",          "Gamepad_FaceButton_Left",   "R",
     "GenericUSBController_Button1"),
    ("IA_ARPG_FaceSouth",         "Gamepad_FaceButton_Bottom", "SpaceBar",
     "GenericUSBController_Button2"),
    ("IA_ARPG_FaceEast",          "Gamepad_FaceButton_Right",  "LeftShift",
     "GenericUSBController_Button3"),

    # The triggers bind DIGITALLY on the raw pad. Both are modifiers -- held or
    # not -- so the analogue travel a DualSense reports is detail this scheme has
    # no use for, and a button edge is cleaner than a threshold.
    ("IA_ARPG_MagicModifier",     "Gamepad_LeftTrigger",       "Q",
     "GenericUSBController_Button7"),
    ("IA_ARPG_DischargeModifier", "Gamepad_RightTrigger",      "E",
     "GenericUSBController_Button8"),

    ("IA_ARPG_SpecialAttack",     "Gamepad_RightShoulder",     "RightMouseButton",
     "GenericUSBController_Button6"),
    ("IA_ARPG_Parry",             "Gamepad_LeftShoulder",      "MiddleMouseButton",
     "GenericUSBController_Button5"),

    # No raw key: a HID D-pad is one hat value, not four buttons. See DPAD_HAT.
    ("IA_ARPG_DPadUp",            "Gamepad_DPad_Up",           "One",    None),
    ("IA_ARPG_DPadDown",          "Gamepad_DPad_Down",         "Two",    None),
    ("IA_ARPG_DPadLeft",          "Gamepad_DPad_Left",         "Three",  None),
    ("IA_ARPG_DPadRight",         "Gamepad_DPad_Right",        "Four",   None),

    ("IA_ARPG_Sprint",            "Gamepad_LeftThumbstick",    "LeftAlt",
     "GenericUSBController_Button11"),
]

# The D-pad as a single 8-way value, decoded by UARPGModalInputComponent. An
# Axis1D action rather than a digital one, because that is what a hat is.
DPAD_HAT = ("IA_ARPG_DPadHat", "GenericUSBController_Axis7")

# Sticks. Each raw axis is one float, so reaching a Vector2D action needs the
# same swizzle the keyboard's WASD mappings use -- there is no 2D key here to
# bind the way Gamepad_Left2D is bound.
#
# Inversion is NOT applied here. It belongs in the device config, where a HID
# quirk -- screen-down reading as positive -- is described once for the device
# rather than compensated for at every place the stick is read.
STICKS = [
    ("/Game/Input/Actions/IA_Move", "GenericUSBController_Axis1", None),
    ("/Game/Input/Actions/IA_Move", "GenericUSBController_Axis2", "SwizzleAxis"),
    ("/Game/Input/Actions/IA_Look", "GenericUSBController_Axis3", None),
    ("/Game/Input/Actions/IA_Look", "GenericUSBController_Axis4", "SwizzleAxis"),
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


def ensure_action(name, value_type=None):
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
    action.set_editor_property(
        "value_type", value_type or unreal.InputActionValueType.BOOLEAN)
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

    def add(action, key_name, modifier=None):
        nonlocal expected
        if not action or not key_name:
            return

        mapping = imc.map_key(action, make_key(key_name))
        if modifier:
            cls = getattr(unreal, "InputModifier" + modifier, None)
            if cls:
                mapping.set_editor_property("modifiers", [unreal.new_object(cls)])
            else:
                log("no modifier InputModifier{}".format(modifier))
        expected += 1

    for name, gamepad_key, keyboard_key, raw_key in ACTIONS:
        action = ensure_action(name)
        if not action:
            log("FAILED to create {} -- skipping".format(name))
            continue

        for key_name in (gamepad_key, keyboard_key, raw_key):
            if not key_name:
                continue

            # MapKey, NOT set_editor_property on the mappings array. There are
            # two arrays on this asset: a deprecated `Mappings` and the
            # `DefaultKeyMappings` struct the engine actually reads. Writing the
            # first produces an asset that looks right, reads back exactly what
            # you wrote, and binds nothing -- and PostLoad only migrates it for
            # assets saved before the change, which a freshly written one is
            # not. MapKey puts it in the right place by construction.
            add(action, key_name)

    hat = ensure_action(DPAD_HAT[0], unreal.InputActionValueType.AXIS1D)
    add(hat, DPAD_HAT[1])

    for action_path, key_name, modifier in STICKS:
        stick_action = unreal.EditorAssetLibrary.load_asset(action_path)
        if not stick_action:
            log("missing {} -- the raw pad will not move".format(action_path))
            continue
        add(stick_action, key_name, modifier)

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
