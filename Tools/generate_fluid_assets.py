"""Creates the fluid and solid definitions the fluid surface subsystem reads.

    "<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" <path>/arpg.uproject \
        -ExecutePythonScript="<path>/Tools/generate_fluid_assets.py" \
        -unattended -nopause -nosplash

RUN THIS BEFORE EXPECTING A PUDDLE. Config/DefaultGame.ini already points
UARPGWorldSettings at the two assets below; until they exist the subsystem logs
that they failed to load and no spell leaves anything on the ground.

WHAT A FLUID DEFINITION IS FOR, and why water needs one at all when
DA_Element_Water already exists: the element says what water IS -- damage,
status, colour, what it combines into. This says what it is like as a BODY LYING
ON THE GROUND: how deep, how fast the sun takes it, how much has to gather
before it stops being a puddle and starts being a lake. Nothing in C++ knows
that water pools and fire does not; the presence of one of these is the whole of
that knowledge.

RE-RUNNABLE. Existing assets are updated in place rather than replaced, so
anything hand-tuned in the editor survives except the fields set here -- it does
not touch SurfaceMaterial, which is authoring work this cannot do.
"""

import unreal

FLUID_DIR = "/Game/ARPG/Fluids"
ELEMENT_DIR = "/Game/ARPG/Magic/Elements"

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def log(message):
    unreal.log("[ARPG fluids] {}".format(message))


def save(asset, path):
    """Saves, and treats a refusal as an error rather than a silent no-op.

    THE RETURN VALUE MATTERS -- see generate_element_assets.py, where verifying
    against the loaded object rather than the file let a broken asset survive
    several "verified" runs.
    """
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
    """Sets a bool property under whichever name the bindings exposed it as.

    UE's Python bindings usually strip the b prefix from a bool -- bStandable
    becomes standable -- but not in every version. Guessing wrong here fails
    silently and leaves a floe you cannot stand on, which is the one thing that
    asset exists to provide, so try both rather than pick.
    """
    for candidate in (name, "b_" + name):
        try:
            asset.set_editor_property(candidate, value)
            return
        except Exception:
            continue

    log("could not set '{}' -- check the property name in the bindings".format(name))


def element(name):
    loaded = unreal.EditorAssetLibrary.load_asset("{}/DA_Element_{}".format(ELEMENT_DIR, name))
    if not loaded:
        log("no DA_Element_{} -- run generate_element_assets.py first".format(name))
    return loaded


# Units are Unreal centimetres, and areas are square centimetres: 2500 is a
# 50cm square, which is about the smallest mark on the ground worth drawing.
#
# WHY THESE NUMBERS. A cast leaves a body roughly the size of the spell that made
# it, and it should be gone in something under a minute so a fight does not
# gradually flood. Evaporation is an inward offset per second, so 1.5 takes a
# 150cm-radius splash to nothing in about a minute and a half of dry weather, and
# rain outruns it three to one.
FLUIDS = [
    {
        "name": "DA_Fluid_Water", "element": "Water",
        # Ankle deep. The surface a floe forms on and a character wades through.
        "depth": 20.0,
        "minimum_area": 2500.0,
        # Past this the body is bottomless and damping -- a spell landing in it
        # hisses instead of exploding. A threshold rather than a flag, so one
        # asset describes a splash and a lake.
        #
        # DELIBERATELY ABOVE WHAT ONE CAST CAN MAKE. A full-charge water burst
        # sweeps roughly 21 square metres, and if that alone crossed the line then
        # a single spell would conjure something the reaction system treats as a
        # river. 80 square metres takes several overlapping casts or an authored
        # body of water, which is the point at which "bottomless" is honest.
        "reservoir_area": 800000.0,
        "energy_per_area": 0.001,
        "rain_growth_rate": 4.5,
        "evaporation_rate": 1.5,
        # Generous on purpose: two casts into the same spot are ONE puddle, and
        # merging late leaves a pair of bodies that each apply their own ambient
        # wetting and each react to the next spell.
        "merge_distance": 120.0,
    },
]

SOLIDS = [
    {
        "name": "DA_Solid_Ice", "element": "Ice",
        "thickness": 30.0,
        # The whole point of the phase gate: a floe two players can stand on.
        "standable": True,
        # Slower than the water under it evaporates, so a frozen pool outlives an
        # unfrozen one -- which is what makes freezing a way to KEEP ground.
        "melt_rate": 0.5,
        "melts_into": "DA_Fluid_Water",
        "minimum_area": 2500.0,
    },
]


def main():
    fluids = {}

    for spec in FLUIDS:
        fluid = ensure(FLUID_DIR, spec["name"], unreal.ARPGFluidDefinition)

        fluid.set_editor_property("element", element(spec["element"]))
        fluid.set_editor_property("depth", spec["depth"])
        fluid.set_editor_property("minimum_area", spec["minimum_area"])
        fluid.set_editor_property("reservoir_area", spec["reservoir_area"])
        fluid.set_editor_property("energy_per_area", spec["energy_per_area"])
        fluid.set_editor_property("rain_growth_rate", spec["rain_growth_rate"])
        fluid.set_editor_property("evaporation_rate", spec["evaporation_rate"])
        fluid.set_editor_property("merge_distance", spec["merge_distance"])

        save(fluid, "{}/{}".format(FLUID_DIR, spec["name"]))
        fluids[spec["name"]] = fluid

    for spec in SOLIDS:
        solid = ensure(FLUID_DIR, spec["name"], unreal.ARPGSolidDefinition)

        solid.set_editor_property("element", element(spec["element"]))
        solid.set_editor_property("thickness", spec["thickness"])
        set_bool(solid, "standable", spec["standable"])
        solid.set_editor_property("melt_rate", spec["melt_rate"])
        solid.set_editor_property("minimum_area", spec["minimum_area"])

        # Melting RETURNS its area to the fluid it came from rather than the
        # water simply vanishing when a floe goes. Null is right for obsidian,
        # which is permanent rock; ice points back at water.
        melts_into = spec.get("melts_into")
        if melts_into:
            solid.set_editor_property("melts_into", fluids[melts_into])

        save(solid, "{}/{}".format(FLUID_DIR, spec["name"]))

    log("{} fluids, {} solids".format(len(FLUIDS), len(SOLIDS)))
    log("Config/DefaultGame.ini already lists these under ARPGWorldSettings.")


main()
