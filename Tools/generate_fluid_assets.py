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


def build_surface(spec):
    material = ensure(MATERIAL_DIR, spec["name"], unreal.Material,
                      unreal.MaterialFactoryNew())

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
        "rain_growth_rate": 4.5,
        "evaporation_rate": 1.5,
        # Generous on purpose: two casts into the same spot are ONE puddle, and
        # merging late leaves a pair of bodies that each apply their own ambient
        # wetting and each react to the next spell.
        "merge_distance": 120.0,
        # Water, and the number every solid's own density is compared against to
        # decide whether it floats. Nothing in the code names either substance.
        "density": 0.001,
        # Thin: runs off anything, arrives quickly, leaves almost nothing.
        "flow_rate": 6.0,
        "yield_slope": 0.0,
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
        "rain_growth_rate": 0.0,
        "evaporation_rate": 0.6,
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

    log("{} surfaces, {} fluids, {} solids".format(len(SURFACES), len(FLUIDS), len(SOLIDS)))
    log("Config/DefaultGame.ini already lists the definitions under ARPGWorldSettings.")


main()
