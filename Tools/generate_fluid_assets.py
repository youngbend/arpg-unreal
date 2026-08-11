"""Creates the fluid and solid definitions, and the surfaces they are drawn with.

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
anything hand-tuned in the editor survives except the fields set here. That
INCLUDES the surface material, so pointing a definition at a real water shader
means changing the surface named in its entry below, not editing the asset and
hoping this never runs again.
"""

import unreal

FLUID_DIR = "/Game/ARPG/Fluids"
MATERIAL_DIR = "/Game/ARPG/Fluids/Materials"
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


def ensure(package_path, name, asset_class, factory=None):
    path = "{}/{}".format(package_path, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        return unreal.EditorAssetLibrary.load_asset(path)

    unreal.EditorAssetLibrary.make_directory(package_path)
    asset = asset_tools.create_asset(
        asset_name=name, package_path=package_path, asset_class=asset_class, factory=factory)
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


# --- Placeholder surfaces ---------------------------------------------------
#
# DELIBERATELY NOT A WATER SHADER. These exist so a puddle is VISIBLE the moment
# the system works, in the same spirit as the placeholder discharge volumes: a
# stand-in that plainly reads as a stand-in, so nobody mistakes it for finished
# and nobody is blocked waiting for art.
#
# What they do buy, and the reason they are not flat colours: a Fresnel drives
# opacity, so the surface is see-through looking straight down and turns opaque
# and bright at grazing angles. That one term is most of what makes a flat plane
# read as liquid rather than as coloured glass, and it needs no textures at all.
#
# The real thing is a normal-map scroll plus refraction and a depth fade, and it
# is a rewrite of the Godot M_ocean shader rather than anything this can author.

SURFACES = [
    {
        "name": "M_ARPG_Water_Placeholder",
        "colour": (0.02, 0.15, 0.28),
        "roughness": 0.04,
        # Barely there face-on, near-solid at a glancing angle.
        "opacity_facing": 0.25,
        "opacity_grazing": 0.9,
        "fresnel_exponent": 4.0,
    },
    {
        "name": "M_ARPG_Ice_Placeholder",
        "colour": (0.55, 0.75, 0.85),
        "roughness": 0.15,
        # Ice is a solid you stand on: mostly opaque, so the floe reads as ground
        # rather than as a pane over the water.
        "opacity_facing": 0.75,
        "opacity_grazing": 1.0,
        "fresnel_exponent": 3.0,
    },
]


def constant(material, value, x, y):
    node = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionConstant, x, y)
    node.set_editor_property("r", value)
    return node


def build_surface(spec):
    material = ensure(MATERIAL_DIR, spec["name"], unreal.Material,
                      unreal.MaterialFactoryNew())

    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)

    # TWO SIDED on purpose. A body is a closed slab, so in principle you never see
    # its backfaces -- but the walls of an eroded hole are wound inward, and a
    # placeholder that vanishes when you look into the gap wastes an afternoon.
    material.set_editor_property("two_sided", True)

    colour = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionVectorParameter, -600, 0)
    colour.set_editor_property("parameter_name", "BaseColour")
    colour.set_editor_property("default_value", unreal.LinearColor(
        spec["colour"][0], spec["colour"][1], spec["colour"][2], 1.0))

    fresnel = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionFresnel, -600, 300)
    fresnel.set_editor_property("exponent", spec["fresnel_exponent"])

    opacity = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionLinearInterpolate, -300, 300)

    unreal.MaterialEditingLibrary.connect_material_expressions(
        constant(material, spec["opacity_facing"], -600, 200), "", opacity, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(
        constant(material, spec["opacity_grazing"], -600, 420), "", opacity, "B")
    unreal.MaterialEditingLibrary.connect_material_expressions(
        fresnel, "", opacity, "Alpha")

    unreal.MaterialEditingLibrary.connect_material_property(
        colour, "", unreal.MaterialProperty.MP_BASE_COLOR)
    unreal.MaterialEditingLibrary.connect_material_property(
        opacity, "", unreal.MaterialProperty.MP_OPACITY)
    unreal.MaterialEditingLibrary.connect_material_property(
        constant(material, spec["roughness"], -600, 560), "",
        unreal.MaterialProperty.MP_ROUGHNESS)
    unreal.MaterialEditingLibrary.connect_material_property(
        constant(material, 1.0, -600, 680), "", unreal.MaterialProperty.MP_SPECULAR)

    unreal.MaterialEditingLibrary.recompile_material(material)
    save(material, "{}/{}".format(MATERIAL_DIR, spec["name"]))

    return material


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
        # High: water is what lightning travels through, and the Conduct row in
        # the combination table is what says so. This is only how well THIS
        # substance carries it -- lava would be near zero.
        "conductivity": 0.85,
        "rain_growth_rate": 4.5,
        "evaporation_rate": 1.5,
        # Generous on purpose: two casts into the same spot are ONE puddle, and
        # merging late leaves a pair of bodies that each apply their own ambient
        # wetting and each react to the next spell.
        "merge_distance": 120.0,
        # Water, and the number every solid's own density is compared against to
        # decide whether it floats. Nothing in the code names either substance.
        "density": 0.001,
        "surface": "M_ARPG_Water_Placeholder",
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
        # Twice water's, so a fireball opens a hole in a floe rather than
        # clearing it: ice takes more energy to shift per unit of ground than the
        # same ground of open water does. This is also what makes a floe
        # meltable AT ALL -- a slab used to carry no energy, so the reaction
        # solver bailed at its zero-energy guard and fire did nothing to ice.
        "energy_per_area": 0.002,
        # 20cm cells. The one resolution knob a floe has: triangles, collision
        # cook and replication all scale with the cell count, and a hole cannot
        # be finer than this. Fine enough that a bowl reads as a bowl.
        "cell_size": 20.0,
        # Narrow and deep enough that one fireball drills through a 30cm slab
        # rather than dishing it.
        "melt_radius": 90.0,
        # --- Floating ---
        #
        # Archimedes, with two knobs traded for feel. Real ice is 0.00092 and
        # floats 92% submerged, leaving a 30cm slab with 2cm proud -- a surface
        # awash rather than one to walk on. 0.0006 rides 18cm under and 12cm
        # proud, which reads as ice and is worth standing on.
        "density": 0.0006,
        "occupant_mass": 80.0,
        # A real person on a 10 square metre floe pushes it down under a
        # centimetre. Openly exaggerated, because the point is that the player
        # FEELS the ice give under them.
        "load_response": 12.0,
        "settle_speed": 3.0,
        # Lags the current rather than matching it: a heavy slab against a fast
        # river does not travel at the water's speed.
        "drift_response": 0.6,
        "surface": "M_ARPG_Ice_Placeholder",
    },
]


def main():
    surfaces = {spec["name"]: build_surface(spec) for spec in SURFACES}
    fluids = {}

    for spec in FLUIDS:
        fluid = ensure(FLUID_DIR, spec["name"], unreal.ARPGFluidDefinition)

        fluid.set_editor_property("element", element(spec["element"]))
        fluid.set_editor_property("depth", spec["depth"])
        fluid.set_editor_property("minimum_area", spec["minimum_area"])
        fluid.set_editor_property("reservoir_area", spec["reservoir_area"])
        fluid.set_editor_property("energy_per_area", spec["energy_per_area"])
        fluid.set_editor_property("conductivity", spec["conductivity"])
        fluid.set_editor_property("rain_growth_rate", spec["rain_growth_rate"])
        fluid.set_editor_property("evaporation_rate", spec["evaporation_rate"])
        fluid.set_editor_property("merge_distance", spec["merge_distance"])
        fluid.set_editor_property("density", spec["density"])
        fluid.set_editor_property("surface_material", surfaces[spec["surface"]])

        save(fluid, "{}/{}".format(FLUID_DIR, spec["name"]))
        fluids[spec["name"]] = fluid

    for spec in SOLIDS:
        solid = ensure(FLUID_DIR, spec["name"], unreal.ARPGSolidDefinition)

        solid.set_editor_property("element", element(spec["element"]))
        solid.set_editor_property("thickness", spec["thickness"])
        set_bool(solid, "standable", spec["standable"])
        solid.set_editor_property("melt_rate", spec["melt_rate"])
        solid.set_editor_property("minimum_area", spec["minimum_area"])
        solid.set_editor_property("energy_per_area", spec["energy_per_area"])
        solid.set_editor_property("cell_size", spec["cell_size"])
        solid.set_editor_property("melt_radius", spec["melt_radius"])
        solid.set_editor_property("density", spec["density"])
        solid.set_editor_property("occupant_mass", spec["occupant_mass"])
        solid.set_editor_property("load_response", spec["load_response"])
        solid.set_editor_property("settle_speed", spec["settle_speed"])
        solid.set_editor_property("drift_response", spec["drift_response"])
        solid.set_editor_property("surface_material", surfaces[spec["surface"]])

        # Melting RETURNS its area to the fluid it came from rather than the
        # water simply vanishing when a floe goes. Null is right for obsidian,
        # which is permanent rock; ice points back at water.
        melts_into = spec.get("melts_into")
        if melts_into:
            solid.set_editor_property("melts_into", fluids[melts_into])

        save(solid, "{}/{}".format(FLUID_DIR, spec["name"]))

    log("{} surfaces, {} fluids, {} solids".format(len(SURFACES), len(FLUIDS), len(SOLIDS)))
    log("Config/DefaultGame.ini already lists the definitions under ARPGWorldSettings.")


main()
