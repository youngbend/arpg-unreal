"""Creates the fire spread medium and a placeholder fuel map.

    "<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" <path>/arpg.uproject \
        -ExecutePythonScript="<path>/Tools/generate_spread_assets.py" \
        -unattended -nopause -nosplash

RUN THIS BEFORE EXPECTING ANYTHING TO BURN. UARPGSpreadSubsystem reads its media
from UARPGWorldSettings.SpreadDefinitions, and until this ran there were no
UARPGSpreadDefinition assets in the project at all -- not one. Phase 9's spread
solver is 1,500 lines that had never executed outside its own automation tests,
because nothing in a real session ever handed it a medium to simulate. The
subsystem does not complain about this: an empty definition list is a legitimate
configuration meaning "this world has no diffusive media", so it initialises,
finds nothing, and sits there.

WHAT A SPREAD DEFINITION IS FOR, given DA_Element_Fire already exists: the
element says what fire IS -- damage type, status, colour, what it combines into.
This says how it MOVES ACROSS GROUND: how far it reaches, how fast it takes
hold, how long a cell of fuel lasts, what it costs to stand in. Nothing in C++
knows that fire spreads and ice does not; the presence of one of these is the
whole of that knowledge, exactly as a fluid definition is the whole of "water
pools".

PLACEHOLDER, AND IT SAYS SO. The numbers below are legible rather than tuned --
round values chosen so the behaviour is obvious when you watch it, not values
anyone balanced. The fuel map is a small hand-baked patch around the origin
rather than a landscape bake. Both are here so the system is OBSERVABLE, in the
same spirit as the placeholder discharge volumes and fluid surfaces: a stand-in
that plainly reads as a stand-in.

RE-RUNNABLE. Existing assets are updated in place rather than replaced, so
anything hand-tuned in the editor survives except the fields set here.
"""

import unreal

SPREAD_DIR = "/Game/ARPG/Spread"
ELEMENT_DIR = "/Game/ARPG/Magic/Elements"

FIRE_NAME = "DA_Spread_Fire"
FUELMAP_NAME = "DA_FuelMap_Placeholder"

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def log(message):
    unreal.log("[ARPG spread] {}".format(message))


def save(asset, path):
    """Saves, and treats a refusal as an error rather than a silent no-op."""
    if unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
        return True

    log("SAVE REFUSED for {} -- is the editor open with this asset loaded?".format(path))
    return False


def ensure(package_path, name, asset_class):
    path = "{}/{}".format(package_path, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        return unreal.EditorAssetLibrary.load_asset(path)

    unreal.EditorAssetLibrary.make_directory(package_path)
    asset = asset_tools.create_asset(
        asset_name=name, package_path=package_path, asset_class=asset_class, factory=None)
    log("created {}".format(path))
    return asset


def set_bool(asset, name, value):
    """Sets a bool under whichever name the bindings exposed it as.

    UE's Python bindings usually strip the b prefix -- bUsesField becomes
    uses_field -- but not in every version, and guessing wrong fails silently.
    """
    for candidate in (name, "b_" + name):
        try:
            asset.set_editor_property(candidate, value)
            return True
        except Exception:
            continue

    log("could not set '{}' -- check the property name in the bindings".format(name))
    return False


def element(name):
    loaded = unreal.EditorAssetLibrary.load_asset("{}/DA_Element_{}".format(ELEMENT_DIR, name))
    if not loaded:
        log("no DA_Element_{} -- run generate_element_assets.py first".format(name))
    return loaded


# --- The fire medium --------------------------------------------------------
#
# ROUND NUMBERS ON PURPOSE. Every one of these is legible at a glance and none is
# balanced; the point is that a fire started at a point visibly creeps outward,
# runs out, and hurts to stand in. Tune later against real ground.
FIRE = {
    # Exposure a cell must accumulate before it catches. 1.0 with a spread rate
    # of 2/s means roughly half a second of contact, which is fast enough to
    # watch propagate and slow enough that brushing past a flame does not ignite.
    "ignition_threshold": 1.0,

    # 400cm is four metres -- about two character widths, so the front advances
    # at a pace the eye can follow rather than jumping across a room.
    "spread_radius": 400.0,
    "spread_rate": 2.0,

    # Negative means "same as spread_rate": a burning object radiates at its
    # neighbours exactly as hard as flame licks across ground. Splitting them is
    # a tuning decision nobody has made yet, so this defers rather than invents.
    "object_spread_rate": -1.0,

    # Half the ignition threshold per second, so a cell that stops receiving
    # recovers in about two seconds. Fire that never cools cannot be escaped.
    "decay_rate": 0.5,

    # Downwind reach is 1.5x upwind at full bias; 0.5 is a visible skew that
    # still lets fire back into the wind.
    "wind_bias": 0.5,

    # A CONSERVED BUDGET, which is what stops a placeholder becoming a world
    # fire. Energy transfers rather than multiplies, so a fire's total reach is
    # bounded by what lit it no matter how flammable the ground is.
    "conserve_energy": True,
    "fuel_energy_yield": 0.1,
    "transfer_keep": 0.5,

    # Eight seconds per full-fuel cell: long enough to watch a front pass and
    # leave scorched ground behind it, short enough that a test fire ends.
    "uses_field": True,
    "fuel_seconds": 8.0,
    "permanent": False,

    # Generous default so unbaked ground burns -- see the header on
    # UARPGSpreadDefinition::DefaultSurfaceFuel. "Burns when it should not" is
    # far easier to notice than "silently fireproof".
    "default_surface_fuel": 1.0,

    # 12 per second is roughly an eighth of a 100-health character per second:
    # standing in fire is clearly a mistake, walking through it is survivable.
    "contact_damage_per_second": 12.0,
}


def author_fire():
    fire = ensure(SPREAD_DIR, FIRE_NAME, unreal.ARPGSpreadDefinition)
    if not fire:
        log("could not create {} -- is ARPGWorld loaded?".format(FIRE_NAME))
        return None

    fire.set_editor_property("element", element("Fire"))

    for key in ("ignition_threshold", "spread_radius", "spread_rate", "object_spread_rate",
                "decay_rate", "wind_bias", "fuel_energy_yield", "transfer_keep",
                "fuel_seconds", "default_surface_fuel", "contact_damage_per_second"):
        fire.set_editor_property(key, FIRE[key])

    set_bool(fire, "conserve_energy", FIRE["conserve_energy"])
    set_bool(fire, "uses_field", FIRE["uses_field"])
    set_bool(fire, "permanent", FIRE["permanent"])

    # THE SURFACE TABLE IS THE INTERESTING HALF, and the reason this asset is
    # worth having rather than just letting DefaultSurfaceFuel cover everything.
    # The ground says WHAT it is; the medium says what that is WORTH. Only the
    # interesting cases need an entry -- everything unlisted falls back.
    #
    # Placeholder projects have no named physical surfaces, so this table maps
    # the raw slots the fuel map below bakes: Default is ordinary ground, and
    # SurfaceType1 is the firebreak. Rename these the day the project defines
    # real surface types in DefaultEngine.ini.
    try:
        fire.set_editor_property("surface_fuel", {
            unreal.PhysicalSurface.SURFACE_TYPE_DEFAULT: 1.0,
            unreal.PhysicalSurface.SURFACE_TYPE1: 0.0,   # stone: a firebreak
        })
    except Exception as error:
        log("could not set surface_fuel ({}) -- the table stays empty, which "
            "means every surface falls back to default_surface_fuel".format(error))

    save(fire, "{}/{}".format(SPREAD_DIR, FIRE_NAME))
    return fire


# --- The fuel map -----------------------------------------------------------
#
# A HAND-BAKED PATCH, not a landscape bake. Four chunks around the origin, each
# ordinary ground except for a vertical stripe of the firebreak surface through
# the middle of chunk (0,0). That one stripe is the whole demonstration: fire
# started to its left should crawl across, stop at the stripe, and go out --
# which is the behaviour that distinguishes a working surface table from a
# uniform world, and it is invisible without a map to provide it.
RESOLUTION = 64
CHUNK_SIZE = 15360.0
FIREBREAK_COLUMNS = range(30, 34)   # four cells wide, through the middle
BAKED_CHUNKS = [(0, 0), (1, 0), (0, 1), (1, 1)]


def author_fuel_map():
    fuel_map = ensure(SPREAD_DIR, FUELMAP_NAME, unreal.ARPGSpreadFuelMap)
    if not fuel_map:
        log("could not create {} -- is ARPGWorld loaded?".format(FUELMAP_NAME))
        return None

    fuel_map.set_editor_property("resolution", RESOLUTION)

    # MUST MATCH the subsystem's own ChunkSize or every lookup silently shears --
    # fuel from one place applied to another, with nothing visibly wrong. 15360
    # is UARPGSpreadSubsystem::ChunkSize's default.
    fuel_map.set_editor_property("chunk_size", CHUNK_SIZE)
    fuel_map.set_editor_property("jitter_range", 0.35)

    default = int(unreal.PhysicalSurface.SURFACE_TYPE_DEFAULT.value)
    firebreak = int(unreal.PhysicalSurface.SURFACE_TYPE1.value)

    for coord in BAKED_CHUNKS:
        cells = [default] * (RESOLUTION * RESOLUTION)

        # The stripe only exists in the origin chunk, so the neighbouring three
        # are plain burnable ground -- there to prove fire crosses a chunk
        # boundary, which is its own thing that can break.
        if coord == (0, 0):
            for y in range(RESOLUTION):
                for x in FIREBREAK_COLUMNS:
                    cells[y * RESOLUTION + x] = firebreak

        fuel_map.set_chunk_cells(unreal.IntPoint(coord[0], coord[1]), cells)
        log("baked chunk {} ({} cells)".format(coord, len(cells)))

    save(fuel_map, "{}/{}".format(SPREAD_DIR, FUELMAP_NAME))
    return fuel_map


def verify(fire, fuel_map):
    """Reads back from the LOADED asset, which is what the game will see.

    Deliberately not trusting the setters' return values: see
    generate_element_assets.py, where verifying against anything else let a
    broken asset survive several "verified" runs.
    """
    ok = True

    if not fire or not fire.get_editor_property("element"):
        log("VERIFY FAILED: {} has no element, so the subsystem will reject it "
            "with a warning and simulate nothing".format(FIRE_NAME))
        ok = False

    if fire and fire.get_editor_property("spread_rate") <= 0.0:
        log("VERIFY FAILED: {} has a zero spread rate".format(FIRE_NAME))
        ok = False

    if fuel_map:
        present = [c for c in BAKED_CHUNKS
                   if fuel_map.has_chunk(unreal.IntPoint(c[0], c[1]))]
        if len(present) != len(BAKED_CHUNKS):
            log("VERIFY FAILED: baked {} chunks but only {} came back".format(
                len(BAKED_CHUNKS), len(present)))
            ok = False

        # The stripe is the point of the map, so check a cell inside it reads
        # back as the firebreak surface rather than ordinary ground.
        middle = (FIREBREAK_COLUMNS[0] + 0.5) / float(RESOLUTION)
        surface = fuel_map.get_surface_at(unreal.IntPoint(0, 0), middle, 0.5)
        if int(surface) != int(unreal.PhysicalSurface.SURFACE_TYPE1.value):
            log("VERIFY FAILED: the firebreak stripe reads back as surface {}".format(surface))
            ok = False

    return ok


def main():
    fire = author_fire()
    fuel_map = author_fuel_map()

    if verify(fire, fuel_map):
        log("OK -- {} and {} written and verified.".format(FIRE_NAME, FUELMAP_NAME))
        log("Both are already referenced from Config/DefaultGame.ini under")
        log("[/Script/ARPGWorld.ARPGWorldSettings].")
    else:
        log("FINISHED WITH FAILURES -- see above. The assets exist but at least")
        log("one field did not take, so do not assume spread works.")


main()
