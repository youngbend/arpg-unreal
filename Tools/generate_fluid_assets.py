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

RE-RUNNABLE. Definitions are updated in place rather than replaced, so anything
hand-tuned in the editor survives except the fields set here. That INCLUDES the
surface material, so pointing a definition at a real water shader means changing
the surface named in its entry below, not editing the asset and hoping this
never runs again.

MATERIALS ARE THE EXCEPTION: their node graphs are torn down and rebuilt from
scratch every run, so hand edits to them do NOT survive. That is deliberate --
the alternative, appending to whatever is already there, stacked a fresh copy of
the graph on top of the old one every time and left duplicate parameters behind.
"""

import os
import struct
import zlib

import unreal

FLUID_DIR = "/Game/ARPG/Fluids"
MATERIAL_DIR = "/Game/ARPG/Fluids/Materials"
ELEMENT_DIR = "/Game/ARPG/Magic/Elements"
EFFECT_DIR = "/Game/ARPG/Magic/Effects"

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


def facets(spec):
    """Builds an FARPGSurfaceFacets from a spec's optional facet keys.

    Absent means smooth, which is the honest reading of a heightfield and what
    ice wants -- see FARPGSurfaceFacets. Only rock asks for anything else.

    The bool inside the struct has the same b-prefix ambiguity set_bool exists
    for, and a struct cannot be built half-way, so each name is tried in turn on
    a struct that is otherwise already filled in.
    """
    style = unreal.ARPGSurfaceFacets()
    style.set_editor_property("relief", spec.get("facet_relief", 0.0))
    style.set_editor_property("spread", spec.get("facet_spread", 0.0))

    style.set_editor_property("grain", spec.get("facet_grain", 60.0))
    return style


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
        # RAISED FROM 0.25. That was chosen for water you look into; the water
        # this game makes is a centimetre or two deep, and at a quarter opacity a
        # real puddle read as a faint stain on the floor rather than as water.
        "opacity_facing": 0.8,
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
    {
        "name": "M_ARPG_Earth_Placeholder",
        "colour": (0.30, 0.22, 0.14),
        "roughness": 0.9,
        # FULLY OPAQUE both ways. Every other surface here is a fluid or a pane
        # of one and reads correctly as translucent; a wall of earth that you can
        # see through is not cover, and cover is the entire point of it.
        #
        # AND THAT MEANS THE BLEND MODE, not just the number. Opacity 1 on a
        # TRANSLUCENT material is still translucent: it writes no depth and sorts
        # per object rather than per pixel, so a slab draws its own far side over
        # its near one and reads as a scrambled box. That is what "opaque" was
        # always meant to say here, and saying it only in the opacity pin is how
        # every surface in this file ended up blended.
        "opaque": True,
        "opacity_facing": 1.0,
        "opacity_grazing": 1.0,
        "fresnel_exponent": 1.0,
    },
    {
        "name": "M_ARPG_Lava_Placeholder",
        "colour": (0.85, 0.20, 0.03),
        "roughness": 0.55,
        # OPAQUE, unlike water. You do not see the ground through molten rock, and
        # a translucent lava pool would read as tinted glass over the floor.
        "opaque": True,
        "opacity_facing": 1.0,
        "opacity_grazing": 1.0,
        "fresnel_exponent": 2.0,
        # And it lights itself. The one surface here that is a light source.
        "emissive": 6.0,
    },
    {
        "name": "M_ARPG_Obsidian_Placeholder",
        "colour": (0.045, 0.04, 0.06),
        # VOLCANIC GLASS: nearly black and nearly a mirror, which is the whole
        # visual difference between it and the earth slab beside it. Both are
        # rock you stand on; only one of them was liquid an instant ago.
        "roughness": 0.08,
        "opaque": True,
        "opacity_facing": 1.0,
        "opacity_grazing": 1.0,
        "fresnel_exponent": 1.0,
    },
]


def constant(material, value, x, y):
    node = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionConstant, x, y)
    node.set_editor_property("r", value)
    return node


def wipe_expressions(material):
    """Empties a material's graph, which delete_all_material_expressions does not.

    THE BULK CALL DELETES ROUGHLY HALF. Measured on 5.8.1: a material holding 52
    expressions has 25 left afterwards, and calling it again halves what remains.
    It is removing from the array it is walking, so every second node is stepped
    over. There is no return value to check -- it hands back None whether it
    emptied the graph or barely touched it.

    Deleting one at a time from a snapshot of the list does work. Verified by
    count, because a partial wipe is invisible: the rebuilt graph is wired to the
    outputs and renders correctly, and all the survivors do is quietly contribute
    a second parameter with the same name and a different default.
    """
    lib = unreal.MaterialEditingLibrary

    for node in list(lib.get_material_expressions(material)):
        lib.delete_material_expression(material, node)

    left = len(lib.get_material_expressions(material))
    if left:
        log("{} still has {} expressions after a wipe".format(
            material.get_name(), left))


def build_surface(spec):
    material = ensure(MATERIAL_DIR, spec["name"], unreal.Material,
                      unreal.MaterialFactoryNew())

    # WIPE BEFORE REBUILDING, because ensure() hands back the material that is
    # already there and create_material_expression only ever ADDS. Without this
    # every re-run stacks another complete copy of the graph on top of the last:
    # the outputs get rewired to the newest copy so the thing still renders, and
    # the only outward sign is a .uasset that grows by ten kilobytes a run. What
    # it costs you is parameters -- two nodes both named FieldTexture with
    # different defaults is ambiguous, and which one the compiler believes is not
    # something you get to know.
    wipe_expressions(material)

    # PER SURFACE, because three of these are rock. A blend mode is not a
    # brightness knob: a translucent material writes no depth and is sorted one
    # whole object at a time, so a solid drawn with one shows its own back faces
    # through its front and interleaves wrongly with every other body near it.
    # Only the ones you are actually meant to see into are blended.
    opaque = spec.get("opaque", False)

    material.set_editor_property(
        "blend_mode",
        unreal.BlendMode.BLEND_OPAQUE if opaque else unreal.BlendMode.BLEND_TRANSLUCENT)

    # TWO SIDED ONLY WHERE YOU SEE THE INSIDE. The original reason was that the
    # walls of an eroded hole were wound inward -- but that was the bug in
    # BuildFieldMesh/BuildSlabMesh, where every face pointed at the body's own
    # interior, and it is fixed: Unreal is left-handed and takes a triangle's
    # normal as (C - A) x (B - A). A closed opaque body now needs no help, and
    # leaving it two-sided would hide the next winding regression the same way
    # this one was hidden. A translucent fluid keeps it, because looking down
    # into a pool genuinely does mean looking at its far wall from inside.
    material.set_editor_property("two_sided", not opaque)

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

    # EMISSIVE, for anything that is its own light source. Molten rock that does
    # not glow reads as mud, and the placeholder's whole job is that you can tell
    # at a glance what you are looking at. Zero for everything else, which is
    # every surface that came before this one.
    glow = spec.get("emissive", 0.0)
    if glow > 0.0:
        emissive = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionMultiply, -300, 800)

        unreal.MaterialEditingLibrary.connect_material_expressions(
            colour, "", emissive, "A")
        unreal.MaterialEditingLibrary.connect_material_expressions(
            constant(material, glow, -600, 860), "", emissive, "B")
        unreal.MaterialEditingLibrary.connect_material_property(
            emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

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
        # Water off a rim is nearly a fall: it sheets down a wall in a moment.
        "wall_speed": 220.0,
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
        # DEPTH PER SECOND, not radius -- see UARPGFluidDefinition's note on the
        # change. Water in the rain gains about half a centimetre a second and
        # loses a fifth of that to the sun, so a puddle left alone lasts a
        # couple of minutes rather than a couple of seconds.
        # MEASURED, NOT GUESSED. At 0.45 and 0.15 a 500-litre pour was gone in
        # eight seconds -- because these are depths per second and they come off
        # the whole wet AREA, and water spreads wide and shallow by design. The
        # comment above always claimed a couple of minutes; it was describing a
        # deep puddle the solver never produces. Same three-to-one ratio, scaled
        # to the depths water actually stands at.
        "rain_growth_rate": 0.06,
        "evaporation_rate": 0.02,
        # Generous on purpose: two casts into the same spot are ONE puddle, and
        # merging late leaves a pair of bodies that each apply their own ambient
        # wetting and each react to the next spell.
        "merge_distance": 120.0,
        # Water, and the number every solid's own density is compared against to
        # decide whether it floats. Nothing in the code names either substance.
        "density": 0.001,
        # Thin: runs off anything, arrives quickly, leaves almost nothing.
        "flow_rate": 6.0,
        # NOT ZERO, WHICH IS WHAT IT WAS. Zero yield means nothing ever stops the
        # spread: a shallow-water solver with no yield stress levels its fluid
        # perfectly, so on a flat floor a puddle keeps going until it is as deep
        # as MinimumFilm everywhere it can reach. Half a cubic metre at half a
        # millimetre is a THOUSAND SQUARE METRES -- far past the 2048cm the sheet
        # draws, so every texel in the window came back wet and the water read as
        # a square centred on the player. Mass was conserved throughout; the
        # model simply had no reason to stop.
        #
        # Real water stops because of surface tension and the roughness of what it
        # is lying on, and this is where that lives -- centimetres of head a cell
        # holds against its neighbour before any of it moves. Well under lava's
        # 2.0, so water still sheets where lava stands, which is the contrast
        # ARPG.World.FluidField.LavaStandsWhereWaterSheets pins down.
        #
        # 0.2 RATHER THAN MORE, and the ceiling is not taste. Above about this,
        # water stops travelling far enough to get round a pillar or bridge two
        # puddles in the fixtures that pin those behaviours down -- so this is
        # very nearly the largest yield the rest of the system still agrees with.
        "yield_slope": 0.2,
        # LEFT ALONE AT HALF A MILLIMETRE, and it was tempting to raise it. What
        # stops the spread is the yield stress above, not this -- and this is what
        # leaves a wet trail behind water running down a slope, so raising it dries
        # the trail up and the water arrives at the bottom of a ramp having never
        # visibly crossed it.
        "minimum_film": 0.05,
        "runoff_batch": 20000.0,
        "surface": "M_ARPG_Water_Placeholder",
    },
    {
        "name": "DA_Fluid_Lava", "element": "Lava",
        # THE SLOWEST THING HERE, and the one this number was added for. Molten rock
        # does not run off a wall, it CREEPS -- a metre of earth wall takes the
        # better part of five seconds, which is the whole read of the material.
        "wall_speed": 22.0,
        # DEEPER THAN WATER, because molten rock does not spread thin: the same
        # volume covers less ground and stands taller on it. Depth is also the
        # exchange rate between volume and area for anything that melts back into
        # this, so obsidian returning to lava would return a smaller pool than the
        # same volume of ice returns as water.
        "depth": 35.0,
        "minimum_area": 2500.0,
        # NO SUCH THING AS A LAVA LAKE from casting. Set far above anything a
        # session could accumulate, because "bottomless and damping" is a
        # statement about a body of water someone authored -- a spell landing in
        # a pool of lava should very much still explode.
        "reservoir_area": 100000000.0,
        # FIVE TIMES WATER. A fireball boils a puddle away; it does nothing to a
        # lava flow, because they are the same substance in temperament and the
        # energy has nowhere to go. This is also what makes it expensive for water
        # to quench: the Quench row spends against this number.
        "energy_per_area": 0.005,
        # NEAR ZERO, exactly as the water definition's own comment predicted. Lava
        # is not the medium a bolt floods across -- and this being data is why
        # nothing in the conduction solver had to learn the difference.
        "conductivity": 0.05,
        # Rain does not grow it, and it does not dry -- it COOLS, which at this
        # system's resolution looks the same as shrinking. Slower than water, so a
        # flow outlasts a puddle and is worth routing around.
        # Rain does nothing to lava, and it cools rather than dries -- slowly.
        "rain_growth_rate": 0.0,
        "evaporation_rate": 0.06,
        "merge_distance": 90.0,
        # Basalt magma, and the number obsidian is compared against to decide
        # whether a crust floats on it. It does not -- see DA_Solid_Obsidian.
        "density": 0.0027,
        # VISCOSITY, as the two numbers it actually takes. An eighth of water's
        # rate makes it arrive late; a yield slope of 2cm per cell makes it STOP,
        # which is the half a rate cannot express and the half that reads as lava.
        # It also stands four times thicker before it will move at all, and leaves
        # far more behind when it finally sets.
        "flow_rate": 0.75,
        "yield_slope": 2.0,
        "minimum_film": 0.4,
        # Bigger dribbles: a slow flow that deposited as often as water would ask
        # for a polygon merge every few frames for a spoonful.
        "runoff_batch": 40000.0,
        "surface": "M_ARPG_Lava_Placeholder",
    },
]

SOLIDS = [
    {
        "name": "DA_Solid_Ice", "element": "Ice",
        "thickness": 30.0,
        # The whole point of the phase gate: a floe two players can stand on.
        "standable": True,
        "breaks_into": "DA_Fluid_Water",
        # Narrow, so one hit drills a floe rather than dishing the whole sheet.
        "break_radius": 90.0,
        "minimum_area": 2500.0,
        # Twice water's, so a fireball opens a hole in a floe rather than
        # clearing it: ice takes more energy to shift per unit of ground than the
        # same ground of open water does. This is also what makes a floe
        # meltable AT ALL -- a slab used to carry no energy, so the reaction
        # solver bailed at its zero-energy guard and fire did nothing to ice.
        "energy_per_area": 0.002,

        # SMOOTH, and by omission rather than by setting anything -- ice froze out
        # of a water surface and its job is to reproduce that surface. See
        # FARPGSurfaceFacets, whose default is exactly this.
        # Narrow and deep enough that one fireball drills through a 30cm slab
        # rather than dishing it.
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

    # --- Earth ---------------------------------------------------------------
    #
    # NOT FROZEN OUT OF ANYTHING, which is what these two exist to demonstrate.
    # Every other solid here is what a fluid became; earth is raised out of the
    # ground by a spell and was never a liquid. The definition does not care --
    # it describes what a slab IS, not where it came from -- and that is the
    # whole reason the slab model was taken off ice.
    #
    # Two of them for one element, because the two discharge types want opposite
    # shapes: Burst throws up a thick pillar to hide behind, Emanate raises a
    # long low wall. Same element, same material, two bodies.
    {
        "name": "DA_Solid_EarthSlab", "element": "Earth",
        # Chest height on a 180cm character: cover you crouch behind, and a step
        # you can climb rather than a cliff.
        "thickness": 120.0,
        "standable": True,

        # BUT FIRE STILL TURNS IT TO LAVA, and this is the pair of independent
        # questions doing its job: time cannot take an earth wall, a spell can,
        # and what a spell leaves behind is molten rock.
        #
        # Naming it here rather than leaving the reaction's own product to do it
        # matters twice. The lava runs down the pillar through the film instead of
        # appearing at its foot, and the reaction ledger stops the product
        # depositing a second helping of the same lava at the same point.
        "breaks_into": "DA_Fluid_Lava",
        # Broader than ice: a fireball scars a pillar rather than boring through it.
        "break_radius": 70.0,

        "minimum_area": 2500.0,

        # DESTRUCTIBLE, THOUGH, and this is the interesting number. MeltRate says
        # time cannot take it; EnergyPerArea says whether a SPELL can. Ten times
        # ice, so a fireball chips a pillar rather than opening a hole in it, but
        # a sustained assault still brings it down -- cover that can be broken is
        # a fight, cover that cannot is a wall the encounter is now behind.
        "energy_per_area": 0.02,


        # ROCK IS NOT A GRID, and drawn honestly from one it reads as masonry --
        # a pillar of perfectly rectangular segments, which is the complaint this
        # answers. The two numbers break the lattice before it is meshed and touch
        # nothing the simulation stores; see FARPGSurfaceFacets.
        #
        # RELIEF IS THE SMALLER HALF. 9cm on a 120cm pillar is a surface with
        # lumps in it, well short of the third of the thickness where a fresh
        # slab starts reading as rubble.
        "facet_relief": 9.0,
        # Boulder-scale lumps on a 2m pillar: a handful across it, not gravel.
        "facet_grain": 70.0,
        # AND SPREAD IS THE HALF THAT MATTERS. Relief alone gives a rectangular
        # grid with a bumpy top, which is still visibly a grid; sliding the
        # samples a third of a cell sideways leaves neither the top nor the
        # silhouette with an axis-aligned edge in it.
        "facet_spread": 0.33,
        # Hard normals, so it reads as planes meeting at edges. Stone fractures;
        # it does not curve.

        # DENSER THAN ANY FLUID HERE, so if one is ever raised in water it rests
        # on the bed instead of bobbing -- one comparison in the same Archimedes
        # the floes use, reached without anything knowing it is rock.
        "density": 0.0025,
        "occupant_mass": 80.0,
        # Rock does not give underfoot. High response, instant settle: both are
        # the buoyancy path being told to do nothing.
        "load_response": 100000.0,
        "settle_speed": 1000.0,
        # And it does not travel, even if the thing it stands in is moving.
        "drift_response": 0.0,
        # Violent. A wall of earth easing gracefully into place is not the spell
        # anyone cast.
        "rise_speed": 14.0,
        "surface": "M_ARPG_Earth_Placeholder",
    },
    {
        "name": "DA_Solid_EarthWall", "element": "Earth",
        # WIDER AND THINNER, which is the whole difference. Waist height: it
        # breaks a charge and blocks a line rather than hiding you.
        "thickness": 60.0,
        "standable": True,
        "breaks_into": "DA_Fluid_Lava",
        # Broader than ice: a fireball scars a pillar rather than boring through it.
        "break_radius": 70.0,
        "minimum_area": 2500.0,
        # Half the pillar's. A wall is thinner, covers far more ground, and
        # should be the thing that comes down first.
        "energy_per_area": 0.01,

        # THE SAME ROCK, and the relief scales with the slab rather than with the
        # cell: this one is half the pillar's thickness, so it gets half the
        # break-up or the wall would read as a heap rather than as something
        # raised. Spread is a FRACTION of a cell already, so it needs no such
        # adjustment -- on 60cm cells it is simply a bigger displacement, which is
        # right for a coarser wall.
        "facet_relief": 5.0,
        "facet_grain": 90.0,
        "facet_spread": 0.33,

        "density": 0.0025,
        "occupant_mass": 80.0,
        "load_response": 100000.0,
        "settle_speed": 1000.0,
        "drift_response": 0.0,
        "rise_speed": 18.0,
        "surface": "M_ARPG_Earth_Placeholder",
    },

    # --- Obsidian ------------------------------------------------------------
    #
    # THE CASE THIS FILE HAS BEEN CITING SINCE IT WAS WRITTEN. Every comment about
    # a solid that is permanent, that melts into nothing, that is denser than the
    # fluid it formed on and therefore does NOT float, has said "obsidian" and
    # meant a thing that did not exist. It exists now, and none of those comments
    # needed changing -- which is the test of whether the slab model was really
    # taken off ice.
    {
        "name": "DA_Solid_Obsidian", "element": "Obsidian",
        # A CRUST, not a raft. Water quenches the SURFACE of a flow; what sets is
        # a skin over molten rock, and the lava is still down there.
        "thickness": 25.0,
        "standable": True,

        "energy_per_area": 0.0,
        # AND NOTHING TO GIVE BACK. Rock that formed on lava is not frozen lava:
        # break it and you get rubble, not a flow. Null is the documented answer
        # and the reason ReturnMeltedFluid checks for it first.
        "breaks_into": None,

        "minimum_area": 2500.0,

        # FACETED, BUT BARELY LIFTED. Volcanic glass fractures into planes, so it
        # wants the hard normals and the broken-up outline as much as earth does
        # -- and it is a 25cm skin, so relief that would read as lumps on a
        # pillar would read as a crust with holes worn in it. Almost all of the
        # break-up here is sideways.
        "facet_relief": 1.5,
        # Finer than earth: volcanic glass fractures small.
        "facet_grain": 35.0,
        "facet_spread": 0.3,

        # LIGHTER THAN THE LAVA UNDER IT, so a crust floats -- which is both what
        # real obsidian does (2.4 against basalt magma's 2.7) and the only thing
        # that looks right: quench the surface of a flow and you should SEE black
        # glass, not watch it sink and leave lava on top.
        #
        # Barely lighter, though, and that is the interesting part. At 89% of the
        # lava's density a 25cm crust rides 22cm under and 3cm proud -- awash,
        # scabbing the surface, nothing like the 12cm of freeboard ice gets. Same
        # Archimedes, and the difference is entirely these two numbers.
        #
        # A slab HEAVIER than what it formed on is still a supported case -- it
        # rests on the bed through GetSurfaceBedAt -- it just is not this one.
        "density": 0.0024,
        "occupant_mass": 80.0,
        # Sitting on the bottom. Neither number is ever read, because the aground
        # branch skips both -- set to the same "do nothing" values earth uses so
        # nobody reads them as tuning.
        "load_response": 100000.0,
        "settle_speed": 1000.0,
        "drift_response": 0.0,
        "surface": "M_ARPG_Obsidian_Placeholder",
    },
]


# --- Earth's three spells ---------------------------------------------------
#
# THE ONE ELEMENT WHOSE DISCHARGE TYPES ARE DIFFERENT SPELLS, rather than the
# same spell in three shapes. Fire is a fireball, a cone and a nova -- one idea
# aimed three ways. Earth throws a rock, raises a pillar and raises a wall, and
# only the first of those is a projectile at all.
#
# NO C++ KNOWS THIS. UARPGMagicElement::DischargeEffects is a map keyed by the
# discharge type, which is the whole dispatch: authoring three entries here is
# authoring three spells. The alternative the Godot version had -- six parallel
# properties and a switch to read them -- is where its per-type bugs lived.
#
# Blueprint subclasses rather than the C++ classes directly, because everything
# that makes one of these a SPELL rather than a mechanism is EditDefaultsOnly:
# which slab it raises, how big, how far in front. Pointing the map at the raw
# class would give a slab effect with no definition, which warns and shoves.
EARTH_EFFECTS = [
    {
        "name": "BP_Earth_Boulder", "parent": "ARPGLaunchSlabProjectile",
        "type": "PROJECT",
        # Short reach: "throw the wall you just raised", not "hunt for ammo".
        "floats": {"reach": 400.0, "max_launch_scale": 2.5,
                   "min_radius": 28.0, "max_radius": 70.0},
    },
    {
        "name": "BP_Earth_Pillar", "parent": "ARPGRaiseSlabEffect",
        "type": "BURST",
        "solid": "DA_Solid_EarthSlab",
        "shape": "PILLAR",
        # Just beyond arm's reach, so it is cover rather than a thing you are
        # standing inside.
        "floats": {"standoff": 220.0, "extent": 160.0, "depth": 90.0,
                   "charge_scale": 1.8},
    },
    {
        "name": "BP_Earth_Wall", "parent": "ARPGRaiseSlabEffect",
        "type": "EMANATE",
        "solid": "DA_Solid_EarthWall",
        # A RING, because an emanation emanates -- the wall comes up all around
        # rather than in front, which is the actual difference between RT+A and
        # RT+X for this element.
        "shape": "RING",
        "floats": {"standoff": 0.0, "extent": 320.0, "depth": 80.0,
                   "charge_scale": 1.5},
    },
]


def blueprint_of(name, parent_class):
    """Creates or loads a Blueprint subclass and returns (asset, cdo)."""
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", parent_class)

    asset = ensure(EFFECT_DIR, name, unreal.Blueprint, factory)
    return asset, unreal.get_default_object(asset.generated_class())


def author_earth(solids):
    """Points DA_Element_Earth at a different spell per discharge type."""
    earth = element("Earth")
    if not earth:
        return

    effects = {}

    for spec in EARTH_EFFECTS:
        parent = getattr(unreal, spec["parent"], None)
        if parent is None:
            log("no {} in the bindings -- compile the project first".format(spec["parent"]))
            return

        asset, cdo = blueprint_of(spec["name"], parent)

        for prop, value in spec["floats"].items():
            cdo.set_editor_property(prop, value)

        if "solid" in spec:
            cdo.set_editor_property("definition", solids[spec["solid"]])

        if "shape" in spec:
            cdo.set_editor_property("shape",
                                    getattr(unreal.ARPGSlabShape, spec["shape"]))

        save(asset, "{}/{}".format(EFFECT_DIR, spec["name"]))
        effects[getattr(unreal.ARPGDischargeType, spec["type"])] = asset.generated_class()

    earth.set_editor_property("discharge_effects", effects)
    save(earth, "{}/DA_Element_Earth".format(ELEMENT_DIR))

    log("earth: {} spells, one per discharge type".format(len(effects)))



# --- The real fluid surfaces ------------------------------------------------
#
# WHAT MAKES THIS DIFFERENT FROM THE PLACEHOLDERS ABOVE: it knows where the
# water is. A placeholder is a colour with a Fresnel on it, which is fine on a
# mesh built from a puddle's own outline and useless on the Niagara sheet, whose
# mesh is the whole simulation window -- a 2048cm square that follows the player.
# Drawn with a placeholder, the sheet is a square of water-coloured ground.
#
# THE MASK COMES FROM THE FIELD TEXTURE, NOT FROM THE SIM. The sheet writes a
# normal-and-height render target of its own, and masking on that would be the
# obvious thing; what it would mean is that the silhouette of every body of water
# in the game depends on what Epic's Grid2D_SW_ComputeNormals happens to put in
# its alpha channel. The field texture is ours, its packing is written down in
# FARPGFluidField::SampleWindow, and there is a test asserting every texel of it
# against the simulation. Mask on the thing you can check.
#
# So the shape is the CPU field's, exactly, and the sheet only ever adds detail
# inside it.
#
# HOW THE MATERIAL FINDS THE TEXTURE. Not through Niagara's material bindings --
# UARPGFluidPresentationSubsystem makes a dynamic instance per element and sets
# the textures on it directly, which is one fewer hand-authored link to get wrong
# and puts the whole path under C++'s control.

FLUID_SURFACES = [
    {
        "name": "M_ARPG_Water",
        "definition": "DA_Fluid_Water",
        "colour": (0.02, 0.15, 0.28),
        "roughness": 0.04,
        "opacity_facing": 0.25,
        "opacity_grazing": 0.92,
        "fresnel_exponent": 4.0,
        # Centimetres of depth over which the edge fades in. A puddle that ended
        # in a hard line at its rim would read as a decal; real shallow water goes
        # transparent as it thins, and that fade IS the edge as far as the eye is
        # concerned.
        #
        # UNDER A CENTIMETRE, because that is the water this game has. Measured
        # from a live pour: a puddle peaks around 2.5cm at the middle and is under
        # 1cm within seconds, so fading over 3cm meant the deepest water on screen
        # never reached a third of its own opacity and the whole body read as a
        # stain. The fade has to be small against the depths that actually occur,
        # not against the depth water would reach if it stood still.
        "edge_fade": 0.6,
    },
    {
        "name": "M_ARPG_Lava",
        "definition": "DA_Fluid_Lava",
        "colour": (0.85, 0.20, 0.03),
        "roughness": 0.55,
        # MASKED RATHER THAN TRANSLUCENT, and it is the blend mode doing the work
        # rather than the colour. You do not see the ground through molten rock,
        # so it wants to be opaque -- but an opaque material has no way to say
        # "not here", and the sheet needs that for every dry cell in the window.
        # Masked is the one mode that is both.
        "masked": True,
        "emissive": 6.0,
        "edge_fade": 1.5,
    },
]


def linear_default_texture():
    """Something the field parameters can compile against.

    A TEXTURE PARAMETER WITH NO DEFAULT IS NOT NEUTRAL. Leave one empty and the
    compiler substitutes /Engine/EngineResources/DefaultTexture, which is sRGB;
    our samplers are declared Linear Color because they carry centimetres and
    world heights rather than colours; the two disagree and the material fails
    to compile outright. What ships then is the ENGINE DEFAULT MATERIAL -- opaque
    grey, no mask, no displacement -- which looks like a bad shader rather than
    like a missing asset, and reads in-game as a grey square following the player
    around. Cost me an evening.

    Four by four, black, uncompressed. Black is the honest default: R=0 is no
    depth and G=0 is a floor at the origin, so a sheet with nothing bound to it
    draws nothing at all rather than a slab of water at sea level.
    """
    path = "{}/T_ARPG_FieldDefault".format(MATERIAL_DIR)

    existing = unreal.EditorAssetLibrary.load_asset(path)
    if existing:
        return configure_linear(existing, path)

    size = 4

    # Written with byte values rather than escapes: this file is source, and a
    # literal NUL in it makes every text tool in the repo call it binary.
    row = bytes([0] + [0, 0, 0, 0] * size)

    def chunk(tag, payload):
        body = tag + payload
        return (struct.pack(">I", len(payload)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xffffffff))

    png = (bytes([137, 80, 78, 71, 13, 10, 26, 10])
           + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(row * size))
           + chunk(b"IEND", b""))

    scratch = os.path.join(
        unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_intermediate_dir()),
        "ARPG_FieldDefault.png")

    with open(scratch, "wb") as handle:
        handle.write(png)

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", scratch)
    task.set_editor_property("destination_path", MATERIAL_DIR)
    task.set_editor_property("destination_name", "T_ARPG_FieldDefault")
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", False)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    texture = unreal.EditorAssetLibrary.load_asset(path)

    if not texture:
        log("could not import T_ARPG_FieldDefault -- fluid materials may not compile")
        return None

    log("T_ARPG_FieldDefault: linear black, the compile-time stand-in for the field")

    return configure_linear(texture, path)


def configure_linear(texture, path):
    """Applied on every run, not just on import.

    SRGB OFF IS THE WHOLE POINT of this asset. UMaterialExpressionTextureBase
    picks the sampler type from the texture, and for an uncompressed RGBA one
    that choice is exactly srgb ? Color : LinearColor -- so a half-finished
    earlier run that left the flag on would break the materials just as surely
    as no texture at all, silently, on a path that reads as already-done.
    """
    texture.set_editor_property("srgb", False)
    texture.set_editor_property(
        "compression_settings",
        unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP)
    texture.set_editor_property("filter", unreal.TextureFilter.TF_NEAREST)

    save(texture, path)

    return texture


def constant2(material, x_value, y_value, x, y):
    node = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionConstant2Vector, x, y)
    node.set_editor_property("r", x_value)
    node.set_editor_property("g", y_value)
    return node


def collection_param(material, mpc, name, x, y):
    node = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionCollectionParameter, x, y)
    node.set_editor_property("collection", mpc)
    node.set_editor_property("parameter_name", name)
    return node


def build_fluid_surface(spec, mpc, default_texture):
    """One element's visible surface: the field's shape, the element's look."""
    material = ensure(MATERIAL_DIR, spec["name"], unreal.Material,
                      unreal.MaterialFactoryNew())

    # See build_surface -- rebuilt from nothing on every run, deliberately.
    wipe_expressions(material)

    masked = spec.get("masked", False)

    material.set_editor_property(
        "blend_mode",
        unreal.BlendMode.BLEND_MASKED if masked else unreal.BlendMode.BLEND_TRANSLUCENT)

    # A fluid surface is seen from one side. The sheet is a horizontal grid and
    # you are above it; two-sided would double the shading cost of every pixel to
    # render an underside nobody is positioned to see.
    material.set_editor_property("two_sided", False)

    # NIAGARA HAS TO BE TOLD, and it does not ask nicely. A material drawn by a
    # mesh renderer must declare this or the engine silently substitutes the
    # DEFAULT material at draw time -- one line in the log, no compile error, and
    # a shader that is perfectly correct and never runs:
    #
    #   Material M_ARPG_Water missing usage flag NiagaraMeshParticles!
    #   Default Material will be used in game.
    #
    # Every fluid surface here exists to be drawn on the sheet, so every one of
    # them needs it.
    set_bool(material, "used_with_niagara_mesh_particles", True)

    lib = unreal.MaterialEditingLibrary

    # --- Where in the field this pixel is -------------------------------------
    #
    # THE SAME ARITHMETIC THE NIAGARA MODULE DOES, in the same units, from the
    # same two numbers -- UARPGFluidPresentationSubsystem publishes FieldOrigin
    # and FieldExtent to the collection every frame, and sets them on the sheet as
    # user parameters. If the shader and the sim ever disagree about where a texel
    # is, it is because one of them stopped reading these.

    world = lib.create_material_expression(
        material, unreal.MaterialExpressionWorldPosition, -1500, 0)

    world_xy = lib.create_material_expression(
        material, unreal.MaterialExpressionComponentMask, -1300, 0)
    world_xy.set_editor_property("r", True)
    world_xy.set_editor_property("g", True)
    world_xy.set_editor_property("b", False)
    world_xy.set_editor_property("a", False)
    lib.connect_material_expressions(world, "", world_xy, "")

    # SET ON THE INSTANCE, NOT LOOKED UP IN A COLLECTION.
    #
    # These were MaterialExpressionCollectionParameter nodes reading
    # MPC_ARPG_Fluid, and a collection node resolves its parameter by GUID rather
    # than by name -- a GUID this script never had a way to set, and which Python
    # does not expose to read back either. An unresolved one compiles to ZERO, so
    # the UV became (world - 0) / 0, every pixel sampled the same texel, and the
    # sheet drew one uniform tint out to a hard square edge with no puddle shape
    # in it and no border fade. Which is exactly what it did.
    #
    # AND IT FIXES A SECOND THING. The collection was published from Tick using
    # last frame's UNSNAPPED centre while the texture was uploaded from this
    # frame's snapped window, so the shader and the picture disagreed by a few
    # centimetres by construction. UARPGFluidPresentationSubsystem::UpdateSheet now
    # sets both on the same instance in the same breath, from the same window.
    origin = lib.create_material_expression(
        material, unreal.MaterialExpressionVectorParameter, -1500, 160)
    origin.set_editor_property("parameter_name", "FieldOrigin")
    origin_xy = lib.create_material_expression(
        material, unreal.MaterialExpressionComponentMask, -1300, 160)
    origin_xy.set_editor_property("r", True)
    origin_xy.set_editor_property("g", True)
    origin_xy.set_editor_property("b", False)
    origin_xy.set_editor_property("a", False)
    lib.connect_material_expressions(origin, "", origin_xy, "")

    offset = lib.create_material_expression(
        material, unreal.MaterialExpressionSubtract, -1100, 80)
    lib.connect_material_expressions(world_xy, "", offset, "A")
    lib.connect_material_expressions(origin_xy, "", offset, "B")

    extent = lib.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -1300, 320)
    extent.set_editor_property("parameter_name", "FieldExtent")
    extent.set_editor_property("default_value", 4096.0)

    uv = lib.create_material_expression(
        material, unreal.MaterialExpressionDivide, -900, 120)
    lib.connect_material_expressions(offset, "", uv, "A")
    lib.connect_material_expressions(extent, "", uv, "B")

    # --- What the simulation says is here -------------------------------------

    field = lib.create_material_expression(
        material, unreal.MaterialExpressionTextureSampleParameter2D, -650, 120)
    field.set_editor_property("parameter_name", "FieldTexture")

    # LINEAR, NOT sRGB. These are centimetres and world heights, not colours --
    # run them through a gamma curve and the water is the wrong depth everywhere,
    # smoothly, with nothing to point at.
    field.set_editor_property(
        "sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)

    if default_texture:
        field.set_editor_property("texture", default_texture)

    lib.connect_material_expressions(uv, "", field, "UVs")

    # R is depth in cm -- see FARPGFluidField::SampleWindow. Zero everywhere the
    # field has no water, which is most of the window, which is the whole point.
    depth = lib.create_material_expression(
        material, unreal.MaterialExpressionComponentMask, -420, 200)
    depth.set_editor_property("r", True)
    depth.set_editor_property("g", False)
    depth.set_editor_property("b", False)
    depth.set_editor_property("a", False)
    lib.connect_material_expressions(field, "", depth, "")

    # --- Where the floor falls away -------------------------------------------
    #
    # THE SHEET IS A HEIGHTFIELD AND A LEDGE IS NOT. One body of water can lie on
    # a platform and on the ground below it at the same time -- measured: a floor
    # spanning Z 0 to Z 210 under a single puddle -- and a grid with one height
    # per cell can only join those by stretching triangles between them. What you
    # get is a two-metre vertical curtain of water standing in the air along every
    # drop, which is the one thing water never does.
    #
    # SO CUT IT RATHER THAN DRAW IT. Sample the floor a texel away in each
    # direction; where it disagrees with this one by more than a small step, this
    # pixel is on the join rather than on the water, and it is not drawn. The
    # water above the ledge and the water below it both stay, with a gap between
    # them -- which is honest, and a great deal better than a fin. A real fall
    # wants a waterfall, and that belongs in the burst system, not here.

    texel = lib.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -900, 420)
    texel.set_editor_property("parameter_name", "FieldTexel")
    texel.set_editor_property("default_value", 1.0 / 256.0)

    here_floor = lib.create_material_expression(
        material, unreal.MaterialExpressionComponentMask, -360, 1080)
    here_floor.set_editor_property("r", False)
    here_floor.set_editor_property("g", True)
    here_floor.set_editor_property("b", False)
    here_floor.set_editor_property("a", False)
    lib.connect_material_expressions(field, "", here_floor, "")

    def neighbour(dx, dy, y):
        """The field one texel over, so the two can be compared."""
        step = lib.create_material_expression(
            material, unreal.MaterialExpressionAppendVector, -760, y)
        lib.connect_material_expressions(
            constant(material, 0.0, -900, y - 40) if dx == 0 else texel, "", step, "A")
        lib.connect_material_expressions(
            constant(material, 0.0, -900, y + 40) if dy == 0 else texel, "", step, "B")

        moved = lib.create_material_expression(
            material, unreal.MaterialExpressionAdd, -640, y)
        lib.connect_material_expressions(uv, "", moved, "A")
        lib.connect_material_expressions(step, "", moved, "B")

        tap = lib.create_material_expression(
            material, unreal.MaterialExpressionTextureSampleParameter2D, -500, y)
        tap.set_editor_property("parameter_name", "FieldTexture")
        tap.set_editor_property(
            "sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)

        if default_texture:
            tap.set_editor_property("texture", default_texture)

        lib.connect_material_expressions(moved, "", tap, "UVs")

        floor = lib.create_material_expression(
            material, unreal.MaterialExpressionComponentMask, -360, y)
        floor.set_editor_property("r", False)
        floor.set_editor_property("g", True)
        floor.set_editor_property("b", False)
        floor.set_editor_property("a", False)
        lib.connect_material_expressions(tap, "", floor, "")

        gap = lib.create_material_expression(
            material, unreal.MaterialExpressionSubtract, -240, y)
        lib.connect_material_expressions(here_floor, "", gap, "A")
        lib.connect_material_expressions(floor, "", gap, "B")

        size = lib.create_material_expression(
            material, unreal.MaterialExpressionAbs, -140, y)
        lib.connect_material_expressions(gap, "", size, "")

        return size

    step_x = neighbour(1, 0, 1180)
    step_y = neighbour(0, 1, 1400)

    steepest = lib.create_material_expression(
        material, unreal.MaterialExpressionMax, -40, 1280)
    lib.connect_material_expressions(step_x, "", steepest, "A")
    lib.connect_material_expressions(step_y, "", steepest, "B")

    # Centimetres of floor step this pixel may straddle before it is a join.
    # Generous against real slopes, tight against a ledge.
    over = lib.create_material_expression(
        material, unreal.MaterialExpressionDivide, 80, 1280)
    lib.connect_material_expressions(steepest, "", over, "A")
    lib.connect_material_expressions(
        constant(material, 25.0, -40, 1360), "", over, "B")

    on_the_join = lib.create_material_expression(
        material, unreal.MaterialExpressionSaturate, 200, 1280)
    lib.connect_material_expressions(over, "", on_the_join, "")

    standing = lib.create_material_expression(
        material, unreal.MaterialExpressionOneMinus, 320, 1280)
    lib.connect_material_expressions(on_the_join, "", standing, "")

    # --- The edge ------------------------------------------------------------

    fade = lib.create_material_expression(
        material, unreal.MaterialExpressionDivide, -220, 200)
    lib.connect_material_expressions(depth, "", fade, "A")
    lib.connect_material_expressions(
        constant(material, spec["edge_fade"], -420, 320), "", fade, "B")

    depth_cover = lib.create_material_expression(
        material, unreal.MaterialExpressionSaturate, -60, 200)
    lib.connect_material_expressions(fade, "", depth_cover, "")

    # --- The edge of the window ----------------------------------------------
    #
    # THE SHEET IS A SQUARE AND THE WATER IS NOT. It draws a fixed window around
    # the viewer, so a body larger than that window gets cut off at the window's
    # border -- a straight edge, at a fixed distance, moving with the player. That
    # is the whole of "a blue square that follows you around": nothing to do with
    # the mask or the material, just the sheet running out.
    #
    # Widening the window pushes it away but cannot remove it. Fading the last
    # tenth can: water still stops at the same place, but it thins out instead of
    # ending, which reads as distance rather than as a wall. Anything beyond the
    # window was never drawn and still is not -- this only stops it announcing
    # exactly where the edge is.

    centred = lib.create_material_expression(
        material, unreal.MaterialExpressionSubtract, -650, 640)
    lib.connect_material_expressions(uv, "", centred, "A")
    lib.connect_material_expressions(
        constant(material, 0.5, -820, 700), "", centred, "B")

    from_middle = lib.create_material_expression(
        material, unreal.MaterialExpressionAbs, -480, 640)
    lib.connect_material_expressions(centred, "", from_middle, "")

    # THE FURTHER OF THE TWO AXES, so the fade follows the square border rather
    # than a circle inscribed in it -- a radial falloff would clip the corners
    # early and leave the flat sides untouched, which is the artefact again.
    reach = lib.create_material_expression(
        material, unreal.MaterialExpressionMax, -330, 640)
    mask_u = lib.create_material_expression(
        material, unreal.MaterialExpressionComponentMask, -410, 700)
    mask_u.set_editor_property("r", True)
    mask_u.set_editor_property("g", False)
    mask_u.set_editor_property("b", False)
    mask_u.set_editor_property("a", False)
    lib.connect_material_expressions(from_middle, "", mask_u, "")

    mask_v = lib.create_material_expression(
        material, unreal.MaterialExpressionComponentMask, -410, 760)
    mask_v.set_editor_property("r", False)
    mask_v.set_editor_property("g", True)
    mask_v.set_editor_property("b", False)
    mask_v.set_editor_property("a", False)
    lib.connect_material_expressions(from_middle, "", mask_v, "")

    lib.connect_material_expressions(mask_u, "", reach, "A")
    lib.connect_material_expressions(mask_v, "", reach, "B")

    # 0.5 is the border. Start fading at 0.45 -- the last tenth of the half-width.
    border = lib.create_material_expression(
        material, unreal.MaterialExpressionSubtract, -200, 640)
    lib.connect_material_expressions(
        constant(material, 0.5, -330, 760), "", border, "A")
    lib.connect_material_expressions(reach, "", border, "B")

    border_fade = lib.create_material_expression(
        material, unreal.MaterialExpressionDivide, -90, 640)
    lib.connect_material_expressions(border, "", border_fade, "A")
    lib.connect_material_expressions(
        constant(material, 0.05, -200, 720), "", border_fade, "B")

    border_cover = lib.create_material_expression(
        material, unreal.MaterialExpressionSaturate, 20, 640)
    lib.connect_material_expressions(border_fade, "", border_cover, "")

    inside = lib.create_material_expression(
        material, unreal.MaterialExpressionMultiply, 140, 300)
    lib.connect_material_expressions(depth_cover, "", inside, "A")
    lib.connect_material_expressions(border_cover, "", inside, "B")

    coverage = lib.create_material_expression(
        material, unreal.MaterialExpressionMultiply, 260, 300)
    lib.connect_material_expressions(inside, "", coverage, "A")
    lib.connect_material_expressions(standing, "", coverage, "B")

    # --- Putting the surface where the water actually is ----------------------
    #
    # THE GRID MESH IS FLAT AND STAYS FLAT. Emitter.GridMesh is generated by the
    # sim, which is easy to mistake for "the sim positions it" -- it does not.
    # MapSimStage1_CreateVertices writes every vertex as
    #
    #     Output11.x = <grid x>;  Output11.y = <grid y>;  Output11.z = Constant;
    #
    # and no later stage touches a vertex position. WaterHeight, which really is
    # BottomContour + WaterDepth, drives the SIMULATION and the normals; it never
    # reaches the geometry. So displacing the surface is the material's job, and
    # if the material does not do it the sheet is a flat plane at whatever height
    # its component sits at -- which is the viewer's own floor, and reads exactly
    # as water glued to the player's feet.
    #
    # FROM THE FIELD, NOT FROM THE SIM. G is the floor and R is the depth, both in
    # world units, both per texel, and both measured straight out of
    # FARPGFluidField::SampleWindow -- the same numbers the mask is built from, so
    # the shape and the height cannot disagree.
    #
    # SCALED BY COVERAGE, which is what makes it safe. The field has no floor to
    # report for ground it has never had water on and fills those texels with the
    # viewer's Z, so an ungated offset threw the dry 93% of the mesh to chest
    # height and left the water in pits underneath. Multiplying by coverage pins
    # dry mesh to its own plane, lifts only what is wet, and eases between the two
    # over the last few millimetres of depth.

    bed = lib.create_material_expression(
        material, unreal.MaterialExpressionComponentMask, -420, 60)
    bed.set_editor_property("r", False)
    bed.set_editor_property("g", True)
    bed.set_editor_property("b", False)
    bed.set_editor_property("a", False)
    lib.connect_material_expressions(field, "", bed, "")

    surface_z = lib.create_material_expression(
        material, unreal.MaterialExpressionAdd, -220, 80)
    lib.connect_material_expressions(bed, "", surface_z, "A")
    lib.connect_material_expressions(depth, "", surface_z, "B")

    here_z = lib.create_material_expression(
        material, unreal.MaterialExpressionComponentMask, -420, -20)
    here_z.set_editor_property("r", False)
    here_z.set_editor_property("g", False)
    here_z.set_editor_property("b", True)
    here_z.set_editor_property("a", False)
    lib.connect_material_expressions(world, "", here_z, "")

    reach_up = lib.create_material_expression(
        material, unreal.MaterialExpressionSubtract, -60, 60)
    lib.connect_material_expressions(surface_z, "", reach_up, "A")
    lib.connect_material_expressions(here_z, "", reach_up, "B")

    # NOT GATED AT ALL ANY MORE, and that is the fix rather than an oversight.
    #
    # This used to be multiplied by the depth coverage, which left every DRY
    # vertex sitting on the component's plane. When a body lies across two levels
    # -- measured, a floor spanning Z 0 to Z 200 under one puddle -- that plane is
    # metres away from the water's own floor, so the surface ramped up from the
    # plane to the water all the way round the body. That ramp is the raised edge,
    # and it is a crown of it on every puddle.
    #
    # THE GATE WAS FOR A PROBLEM THAT NO LONGER EXISTS. It was added when the
    # sheet was one window centred on the player and the field filled unknown
    # ground with the VIEWER'S Z -- ungated, that threw the dry mesh to chest
    # height. A sheet is now the size of one body, unknown ground reports THAT
    # BODY'S floor, and the floor is dilated a texel past the water, so G is a
    # sane height everywhere on the grid. Draping the whole sheet onto it makes
    # the dry mesh lie on the ground where it belongs and leaves the water's edge
    # a couple of centimetres proud, which is what an edge should be.
    lift = reach_up

    # Z ONLY. Moving a vertex sideways slides it onto a different texel, which
    # moves it again next frame -- the surface would crawl rather than settle.
    flat = lib.create_material_expression(
        material, unreal.MaterialExpressionAppendVector, 160, 40)
    lib.connect_material_expressions(
        constant2(material, 0.0, 0.0, 40, -40), "", flat, "A")
    lib.connect_material_expressions(lift, "", flat, "B")

    lib.connect_material_property(
        flat, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

    # --- The look ------------------------------------------------------------

    colour = lib.create_material_expression(
        material, unreal.MaterialExpressionVectorParameter, -650, -220)
    colour.set_editor_property("parameter_name", "BaseColour")
    colour.set_editor_property("default_value", unreal.LinearColor(
        spec["colour"][0], spec["colour"][1], spec["colour"][2], 1.0))

    lib.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR)
    lib.connect_material_property(
        constant(material, spec["roughness"], -650, -60), "",
        unreal.MaterialProperty.MP_ROUGHNESS)
    lib.connect_material_property(
        constant(material, 1.0, -650, 20), "", unreal.MaterialProperty.MP_SPECULAR)

    if masked:
        # Nothing between "there is lava here" and "there is not".
        lib.connect_material_property(
            coverage, "", unreal.MaterialProperty.MP_OPACITY_MASK)
    else:
        # COVERAGE TIMES FRESNEL. The Fresnel is what makes a flat plane read as
        # liquid at all -- see-through looking straight down, bright and opaque at
        # a grazing angle -- and the coverage is what stops it being a square.
        fresnel = lib.create_material_expression(
            material, unreal.MaterialExpressionFresnel, -650, 420)
        fresnel.set_editor_property("exponent", spec["fresnel_exponent"])

        look = lib.create_material_expression(
            material, unreal.MaterialExpressionLinearInterpolate, -420, 420)
        lib.connect_material_expressions(
            constant(material, spec["opacity_facing"], -650, 560), "", look, "A")
        lib.connect_material_expressions(
            constant(material, spec["opacity_grazing"], -650, 640), "", look, "B")
        lib.connect_material_expressions(fresnel, "", look, "Alpha")

        opacity = lib.create_material_expression(
            material, unreal.MaterialExpressionMultiply, -220, 380)
        lib.connect_material_expressions(look, "", opacity, "A")
        lib.connect_material_expressions(coverage, "", opacity, "B")

        lib.connect_material_property(
            opacity, "", unreal.MaterialProperty.MP_OPACITY)

    glow = spec.get("emissive", 0.0)
    if glow > 0.0:
        emissive = lib.create_material_expression(
            material, unreal.MaterialExpressionMultiply, -220, 700)
        lib.connect_material_expressions(colour, "", emissive, "A")
        lib.connect_material_expressions(
            constant(material, glow, -420, 780), "", emissive, "B")
        lib.connect_material_property(
            emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    # --- Declared, not wired --------------------------------------------------
    #
    # NormalRT and VelocityRT are what the sheet's own simulation produces, and
    # they are the whole reason there is a sheet rather than a flat quad. They are
    # NOT connected yet, deliberately: what Epic's Grid2D_SW_ComputeNormals packs
    # into those channels has not been verified, and a normal reconstructed from
    # the wrong packing gives a surface lit from an impossible direction -- which
    # looks like a broken shader rather than like a wrong assumption, and is
    # correspondingly hard to trace back.
    #
    # Declared so C++ can bind them and the plumbing is provably in place before
    # anybody has to reason about the contents. Wire them once the sheet is
    # visibly running and there is something to compare against.
    for name, y in (("NormalRT", 900), ("VelocityRT", 1040)):
        node = lib.create_material_expression(
            material, unreal.MaterialExpressionTextureObjectParameter, -650, y)
        node.set_editor_property("parameter_name", name)
        node.set_editor_property(
            "sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)

        if default_texture:
            node.set_editor_property("texture", default_texture)

    lib.recompile_material(material)
    save(material, "{}/{}".format(MATERIAL_DIR, spec["name"]))

    return material


def build_fluid_surfaces():
    """The two real surfaces, and pointing the fluid definitions at them."""
    mpc = unreal.EditorAssetLibrary.load_asset(
        "/Game/ARPG/Fluids/Niagara/MPC_ARPG_Fluid")

    if not mpc:
        log("NO MPC_ARPG_Fluid -- run Tools/generate_fluid_niagara_assets.py first.")
        log("Skipping the real fluid surfaces; the placeholders stay in place.")
        return

    default_texture = linear_default_texture()

    for spec in FLUID_SURFACES:
        material = build_fluid_surface(spec, mpc, default_texture)

        path = "{}/{}".format(FLUID_DIR, spec["definition"])
        definition = unreal.EditorAssetLibrary.load_asset(path)

        if not definition:
            log("no {} to point at {}".format(spec["definition"], spec["name"]))
            continue

        definition.set_editor_property("surface_material", material)
        save(definition, path)
        log("{} now draws with {}".format(spec["definition"], spec["name"]))


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
        fluid.set_editor_property("flow_rate", spec["flow_rate"])
        fluid.set_editor_property("yield_slope", spec["yield_slope"])
        fluid.set_editor_property("minimum_film", spec["minimum_film"])
        fluid.set_editor_property("runoff_batch", spec["runoff_batch"])
        fluid.set_editor_property("wall_speed", spec.get("wall_speed", 200.0))
        fluid.set_editor_property("surface_material", surfaces[spec["surface"]])

        save(fluid, "{}/{}".format(FLUID_DIR, spec["name"]))
        fluids[spec["name"]] = fluid

    solids = {}

    for spec in SOLIDS:
        solid = ensure(FLUID_DIR, spec["name"], unreal.ARPGSolidDefinition)

        solid.set_editor_property("element", element(spec["element"]))
        solid.set_editor_property("thickness", spec["thickness"])
        set_bool(solid, "standable", spec["standable"])
        solid.set_editor_property("minimum_area", spec["minimum_area"])
        solid.set_editor_property("energy_per_area", spec["energy_per_area"])
        solid.set_editor_property("facets", facets(spec))
        solid.set_editor_property("break_radius", spec.get("break_radius", 90.0))
        solid.set_editor_property("density", spec["density"])
        solid.set_editor_property("occupant_mass", spec["occupant_mass"])
        solid.set_editor_property("load_response", spec["load_response"])
        solid.set_editor_property("settle_speed", spec["settle_speed"])
        solid.set_editor_property("drift_response", spec["drift_response"])
        solid.set_editor_property("rise_speed", spec.get("rise_speed", 14.0))
        solid.set_editor_property("surface_material", surfaces[spec["surface"]])

        # Melting RETURNS its area to the fluid it came from rather than the
        # water simply vanishing when a floe goes. Null is right for obsidian,
        # which is permanent rock; ice points back at water.
        breaks_into = spec.get("breaks_into")
        if breaks_into:
            solid.set_editor_property("breaks_into", fluids[breaks_into])

        save(solid, "{}/{}".format(FLUID_DIR, spec["name"]))
        solids[spec["name"]] = solid

    # AFTER the solids, because two of earth's three spells name one. This is the
    # only place the ordering matters and it is the reason this lives here rather
    # than in the element script, which runs first and could not see them.
    author_earth(solids)

    # AFTER the definitions, because it repoints two of them. The placeholders
    # above stay: three of the five are solids and are drawn by their own meshes,
    # which is what a placeholder Fresnel is actually adequate for.
    build_fluid_surfaces()

    log("{} surfaces, {} fluids, {} solids".format(len(SURFACES), len(FLUIDS), len(SOLIDS)))
    log("Config/DefaultGame.ini already lists the definitions under ARPGWorldSettings.")


main()
