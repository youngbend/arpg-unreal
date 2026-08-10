"""Creates the Input Action assets and mapping context the modal scheme needs.

Run from anywhere:

    "<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" <path>/arpg.uproject \
        -ExecutePythonScript="<path>/Tools/generate_input_assets.py" \
        -unattended -nopause -nosplash

CLOSE THE EDITOR FIRST. A package whose file is held open cannot be written, and
that failure is only a warning -- see save() below.

RE-RUNNABLE. Existing actions are reused so hand edits to them survive; the
mapping list is rebuilt every time, because it is derived rather than authored
and an asset carrying entries from a previous layout is worse than no asset --
the stale ones still bind.

WHY A SCRIPT AND NOT HAND-AUTHORING. Fourteen actions with up to three keys each
is a lot of clicking to get exactly right, and the layout is not arbitrary: it
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

# Sticks, as (action path, raw key, modifier class names).
#
# EACH RAW AXIS IS ONE FLOAT, and Move and Look are both 2D actions -- so an
# unmodified axis lands on X. The second axis of each stick therefore needs the
# same swizzle the keyboard's WASD mappings use; without it both axes pile onto
# X, and forward/back and pitch quietly stop existing while left/right still
# works -- which reads as a broken device rather than a missing modifier.
#
# A DEADZONE ON EVERY AXIS, and not for tidiness: a HID stick reports its rest
# position as a small non-zero number, and Enhanced Input treats any non-zero
# value as input -- so without one the character walks slowly forever and the
# camera drifts. The XInput path gets this free from the template's own mapping.
#
# INVERSION IS NOT HERE. It belongs in the device config, where a HID quirk --
# screen-down reading as positive -- is described once for the device rather
# than compensated for at every place the stick is read.
STICKS = [
    ("/Game/Input/Actions/IA_Move", "GenericUSBController_Axis1", ["DeadZone"]),
    ("/Game/Input/Actions/IA_Move", "GenericUSBController_Axis2", ["DeadZone", "SwizzleAxis"]),
    ("/Game/Input/Actions/IA_Look", "GenericUSBController_Axis3", ["DeadZone"]),
    ("/Game/Input/Actions/IA_Look", "GenericUSBController_Axis4", ["DeadZone", "SwizzleAxis"]),
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


def make_mapping(action, key_name, modifier_names=None, outer=None):
    """One complete mapping, BUILT rather than retrieved and edited.

    Structs cross the Python boundary BY VALUE, so anything handed back -- from
    MapKey, or from iterating an existing mapping array -- is a copy, and setting
    properties on it mutates a temporary that is then discarded. The asset saves,
    reads back with every mapping present, and the modifiers are simply absent.

    Assembling each struct here and assigning the whole array in one go is the
    only shape of this that persists.

    OUTER IS REQUIRED FOR THE MODIFIERS. A modifier is an instanced sub-object,
    and one created with no owner is transient -- it survives in memory, so the
    array comes back the right LENGTH, and every entry deserialises as null. The
    mapping context has to own them for them to be written at all.
    """
    mapping = unreal.EnhancedActionKeyMapping()
    mapping.set_editor_property("action", action)
    mapping.set_editor_property("key", make_key(key_name))

    instances = []
    for name in (modifier_names or []):
        cls = getattr(unreal, "InputModifier" + name, None)
        if cls:
            instances.append(unreal.new_object(cls, outer=outer))
        else:
            log("no modifier InputModifier{}".format(name))

    if instances:
        mapping.set_editor_property("modifiers", instances)

    return mapping


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

    # Boolean unless told otherwise: every control in the modal scheme is a
    # press or a hold. The hat is the one exception, and it says so.
    action.set_editor_property(
        "value_type", value_type or unreal.InputActionValueType.BOOLEAN)
    save(action, path)
    log("created {}".format(path))
    return action


def main():
    unreal.EditorAssetLibrary.make_directory(CONTENT_DIR)

    imc_path = "{}/{}".format(CONTENT_DIR, IMC_NAME)

    # OPENED FIRST, before any mapping is built, because it has to be the owner
    # of every modifier -- see make_mapping.
    #
    # EMPTIED AND REFILLED rather than deleted and recreated: a package deleted
    # in this session still holds its name, so the recreate fails and you are
    # left with nothing at all.
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

    mappings = []
    expected_modifiers = 0

    for name, gamepad_key, keyboard_key, raw_key in ACTIONS:
        action = ensure_action(name)
        if not action:
            log("FAILED to create {} -- skipping".format(name))
            continue

        for key_name in (gamepad_key, keyboard_key, raw_key):
            if key_name:
                mappings.append(make_mapping(action, key_name))

    hat = ensure_action(DPAD_HAT[0], unreal.InputActionValueType.AXIS1D)
    if hat:
        mappings.append(make_mapping(hat, DPAD_HAT[1]))

    for action_path, key_name, modifier_names in STICKS:
        stick_action = unreal.EditorAssetLibrary.load_asset(action_path)
        if not stick_action:
            log("missing {} -- the raw pad will not move".format(action_path))
            continue
        mappings.append(make_mapping(stick_action, key_name, modifier_names, imc))
        expected_modifiers += 1

    # DefaultKeyMappings, NOT the `Mappings` array beside it. That one is
    # deprecated as of 5.7 and the engine no longer reads it -- writing there
    # produces an asset that looks correct in every way, reads back exactly what
    # you wrote, and binds nothing. The tell is that an IMC authored in the
    # editor reports an EMPTY Mappings array, because its real data lives here.
    data = unreal.InputMappingContextMappingData()
    data.set_editor_property("mappings", mappings)
    imc.set_editor_property("default_key_mappings", data)

    # Emptied so nothing is left in the dead property to mislead the next person
    # who opens this asset and sees a full-looking list that binds nothing.
    imc.set_editor_property("mappings", [])

    if not save(imc, imc_path):
        log("{} was NOT written. Close the editor and run this again.".format(IMC_NAME))
        return

    # --- verification -------------------------------------------------------
    #
    # Counted in two parts, because they have failed separately: once with every
    # mapping missing, and once with every mapping present and every modifier
    # gone. A single total would have reported success both times.
    live = imc.get_editor_property("default_key_mappings").get_editor_property("mappings")
    # `is not None` on each entry, not merely a non-empty list: a transient
    # modifier deserialises as null and would otherwise count as present.
    with_modifiers = sum(
        1 for m in live
        if all(x is not None for x in m.get_editor_property("modifiers"))
        and m.get_editor_property("modifiers"))

    log("{}: {} mappings (expected {}), {} with modifiers (expected {})".format(
        IMC_NAME, len(live), len(mappings), with_modifiers, expected_modifiers))

    if len(live) != len(mappings) or with_modifiers != expected_modifiers:
        log("MISMATCH -- re-read the asset in a fresh session before trusting this")


main()
