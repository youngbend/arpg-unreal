"""Builds everything about the fluid sheet that a script can build.

    "<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" <path>/arpg.uproject \
        -ExecutePythonScript="<path>/Tools/generate_fluid_niagara_assets.py" \
        -unattended -nopause -nosplash

WHAT IS AND IS NOT SCRIPTABLE, measured rather than assumed. unreal.NiagaraSystem
exposes thirty properties and not one of them is an emitter, a module, or a
parameter; unreal.NiagaraEmitter exposes its UPROPERTYs and no way into its
graph. So NODES are out of reach, permanently, and anybody who tries again will
find the same thing.

Everything AROUND the nodes is reachable, and that turns out to be most of the
work:

    DUPLICATION      EditorAssetLibrary.duplicate_asset. The fork of the engine's
                     beta template content into /Game is a hundred lines of list.
    CREATION         NiagaraModuleScriptFactory makes the two modules we have to
                     write ourselves, so they exist under the right names before
                     anybody opens the editor.
    VERIFICATION     NiagaraFunctionLibrary.get_all_user_parameters reads a
                     system's user parameters -- which is the contract with C++,
                     and the one thing in this whole area that nothing else
                     checks. See report_contract.

WHAT IS LEFT FOR A PERSON, and it is worth being exact because "some Niagara
work" is not an actionable handover:

    1. The two module GRAPHS this script creates empty.
    2. Repointing NS_ARPG_FluidSheet's emitter at the forked modules rather than
       the engine's. Duplication cannot do this: a copy keeps the references the
       original had, and the only Python tool that rewrites references --
       consolidate_assets -- would delete the engine asset it redirected away
       from, inside the user's engine install.
    3. Declaring the nine user parameters. Run this script again afterwards and
       it will tell you which ones are still missing.

Docs/FLUID_NIAGARA_AUTHORING.md is the long form of all three.

RE-RUNNABLE. Nothing is overwritten: an asset that already exists is left exactly
as it is, which is what makes it safe to run again after hand-editing to find out
what is still outstanding.
"""

import unreal

FLUID_DIR = "/Game/ARPG/Fluids"
NIAGARA_DIR = "/Game/ARPG/Fluids/Niagara"
MODULE_DIR = NIAGARA_DIR + "/Modules"

# Where the beta template content lives. Mount point confirmed by probe.
ENGINE_SW = "/NiagaraFluids/Modules/Grid2D/ShallowWater"
ENGINE_SYSTEMS = "/NiagaraFluids/Templates/Liquid/2D/Systems/ShallowWater"
ENGINE_EMITTERS = "/NiagaraFluids/Templates/Liquid/2D/Emitters"

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

REPORT = []


def log(message):
    REPORT.append(message)
    unreal.log("[ARPG fluid niagara] {}".format(message))


def save(path):
    """Saves, and treats a refusal as an error rather than a silent no-op.

    THE RETURN VALUE MATTERS -- see generate_element_assets.py, where verifying
    against the loaded object rather than the file let a broken asset survive
    several "verified" runs.
    """
    if unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
        return True

    log("SAVE REFUSED for {} -- is the editor open with this asset loaded?".format(path))
    return False


# --- Forking the beta content ----------------------------------------------
#
# WHY COPY AT ALL. NiagaraFluids.uplugin carries IsBetaVersion=true, and
# Content/Templates is demo content Epic reshapes between releases. A system
# built directly on it can change under the project on an engine upgrade, and the
# symptom is water that looks different with no commit to blame. The cost of
# forking is not getting Epic's fixes for free, which is the right trade for a
# game and the wrong one for a sample.
#
# TWO OF THEM ARE DELIBERATELY ABSENT. Grid2D_Init_BottomCapture and
# Grid2D_SW_ComputeBottomContour grab the terrain with a scene capture: an extra
# render pass, a frame of latency, and an answer that can disagree with the
# simulation. We already have the truth on the CPU and hand it over as a texture,
# so ARPG_SW_ReadFieldTexture replaces both. Not copying them is not an oversight
# and they should not be added back.

FORKED_MODULES = [
    # The solver itself.
    "Grid2D_SW_Advect",
    "Grid2D_SW_UpdateVelocity",
    "Grid2D_SW_UpdateDepthGrid",
    "Grid2D_SW_UpdateSimAttributes",
    "Grid2D_SW_Init",

    # Keeping it stable and keeping its edges honest.
    "Grid2D_SW_FilterOvershoot",
    "Grid2D_SW_SmoothBoundaryCells",
    "Grid2D_SW_ExtrapolateHeight",
    "Grid2D_SW_ExtrapolateRenderHeight",
    "Grid2D_SW_ExtrapolateVelocities",

    # What it looks like.
    "Grid2D_SW_ComputeNormals",
    "Grid2D_SW_FoamSpread",

    # Things going into it.
    "Grid2D_SW_Source",
    "Grid2D_SW_UpdateCollisions",
    "Grid2D_SW_CollisionForce",
    "Grid2D_UpdateCollisions",

    # The window following the player.
    "Grid2D_SW_MovingWindow_GetDelta",
    "Grid2D_SW_MovingWindow_ShiftWaterGrid",
    "Grid2D_SW_MovingWindow_ShiftVelocityGrid",
    "Grid2D_SW_DistributeOverlapVolume",

    # Worth having while the thing is being built.
    "Grid2D_SW_DebugVisualization",
]

# The two the scene capture is replaced by, named so the reason survives.
REPLACED_MODULES = [
    "Grid2D_Init_BottomCapture",
    "Grid2D_SW_ComputeBottomContour",
]


def fork(source, package_path, name):
    """Copies one asset into the project, leaving any existing copy alone."""
    destination = "{}/{}".format(package_path, name)

    if unreal.EditorAssetLibrary.does_asset_exist(destination):
        return destination, False

    if not unreal.EditorAssetLibrary.does_asset_exist(source):
        log("MISSING SOURCE {} -- has the engine's NiagaraFluids content moved?".format(source))
        return None, False

    unreal.EditorAssetLibrary.make_directory(package_path)

    if not unreal.EditorAssetLibrary.duplicate_asset(source, destination):
        log("COULD NOT COPY {} -> {}".format(source, destination))
        return None, False

    save(destination)
    return destination, True


def fork_modules():
    made = 0
    had = 0

    for name in FORKED_MODULES:
        path, created = fork("{}/{}".format(ENGINE_SW, name), MODULE_DIR, name)

        if path is None:
            continue

        made += 1 if created else 0
        had += 0 if created else 1

    log("modules: {} copied, {} already forked, {} deliberately not copied ({})".format(
        made, had, len(REPLACED_MODULES), ", ".join(REPLACED_MODULES)))


# --- The two we have to write ourselves ------------------------------------
#
# CREATED EMPTY, ON PURPOSE. A script cannot fill in a Niagara graph, but it can
# make sure the assets exist under the names C++ and the docs already use -- so
# the person doing the graph work opens an asset that is already in the right
# place with the right name, instead of creating it and getting one of those
# wrong. The bodies are described in Docs/FLUID_NIAGARA_AUTHORING.md.

CUSTOM_MODULES = [
    {
        "name": "ARPG_SW_ReadFieldTexture",
        "purpose": "samples FieldTexture and writes Bottom Contour from its B channel, "
                   "replacing the scene-capture pair above",
    },
    {
        "name": "ARPG_SW_ConstrainToField",
        "purpose": "blends Water Height back towards what the CPU field says, at "
                   "ConstrainRate -- the module that makes GPU detail over CPU truth work",
    },
]


def custom_modules():
    for spec in CUSTOM_MODULES:
        path = "{}/{}".format(MODULE_DIR, spec["name"])

        if unreal.EditorAssetLibrary.does_asset_exist(path):
            log("{} already exists -- left alone".format(spec["name"]))
            continue

        unreal.EditorAssetLibrary.make_directory(MODULE_DIR)

        asset = asset_tools.create_asset(
            asset_name=spec["name"],
            package_path=MODULE_DIR,
            asset_class=unreal.NiagaraScript,
            factory=unreal.NiagaraModuleScriptFactory())

        if not asset:
            log("COULD NOT CREATE {}".format(spec["name"]))
            continue

        save(path)
        log("created EMPTY module {} -- {}".format(spec["name"], spec["purpose"]))


# --- The sheet -------------------------------------------------------------

def fork_sheet():
    emitter, _ = fork("{}/ShallowWater_Emitter".format(ENGINE_EMITTERS),
                      NIAGARA_DIR, "E_ARPG_FluidSheet")

    if emitter is None:
        # The plugin has shipped this under two names across versions.
        emitter, _ = fork("{}/ShallowWaterEmitter".format(ENGINE_EMITTERS),
                          NIAGARA_DIR, "E_ARPG_FluidSheet")

    system, created = fork("{}/Grid2D_SW_Pool".format(ENGINE_SYSTEMS),
                           NIAGARA_DIR, "NS_ARPG_FluidSheet")

    if system:
        log("sheet: NS_ARPG_FluidSheet {}".format(
            "forked from Grid2D_SW_Pool" if created else "already present"))

    return system


# --- The contract, checked rather than hoped -------------------------------
#
# THE ONE THING NOTHING ELSE CHECKS. Every name below is a string that has to
# match something a person typed into a graph; a misspelling is a silent no-op at
# runtime rather than a compile error, and the symptom is a sheet that draws
# nothing with no error anywhere. The same list lives in
# ARPGFluidPresentationSubsystem.cpp under SheetParams.

REQUIRED_PARAMETERS = [
    "WorldGridSize",
    "ResolutionMaxAxis",
    "NormalRT",
    "VelocityRT",
    "FieldTexture",
    "FieldOrigin",
    "FieldExtent",
    "ConstrainRate",
    "SurfaceMaterial",
]


def report_contract(system_path):
    if not system_path:
        return

    system = unreal.EditorAssetLibrary.load_asset(system_path)

    if not system:
        return

    try:
        found = unreal.NiagaraFunctionLibrary.get_all_user_parameters(system)
    except Exception as error:
        log("could not read user parameters ({}) -- check by hand".format(error))
        return

    names = set()

    for entry in found:
        # The binding's own name, however this engine version spells the field.
        for attribute in ("name", "parameter_name", "variable_name"):
            try:
                names.add(str(entry.get_editor_property(attribute)))
                break
            except Exception:
                continue
        else:
            names.add(str(entry))

    missing = [p for p in REQUIRED_PARAMETERS if p not in names]

    if missing:
        log("USER PARAMETERS STILL TO DECLARE on NS_ARPG_FluidSheet: {}".format(
            ", ".join(missing)))
    else:
        log("all nine user parameters are present -- C++ can drive the sheet")


# --- What each parameter collection has to carry ---------------------------
#
# THREE NUMBERS AND TWO POSITIONS, and that is genuinely all of it. The grids
# themselves are render targets bound straight onto the Niagara component from
# C++; the only thing a material cannot work out for itself is WHERE IN THE WORLD
# the corner of those grids currently sits, because the window follows the player
# and moves every frame.
#
# THE FIRST THREE NAMES ARE EPIC'S, NOT OURS, and deliberately.
# UShallowWaterSettings defaults GridCenterMPCName to "SimLocation",
# WorldGridSizeMPCName to "FluidSimSize" and ResolutionMaxAxisMPCName to
# "FluidSimResolution". Every material function Epic ships under
# NiagaraFluids/Content/Materials/ShallowWater reads those three, so matching
# them means those functions drop into our surface material unmodified -- and the
# alternative was re-deriving somebody else's water shader for nicer spelling.

SCALAR_PARAMETERS = [
    {"name": "FluidSimSize", "value": 2048.0},
    {"name": "FluidSimResolution", "value": 512.0},
    {
        "name": "FieldExtent",
        "value": 2048.0,
        # THE CPU FIELD'S WINDOW, which is a different thing from the sheet's even
        # when the numbers agree. A material that wants to fade the surface out
        # where the water actually ends reads the field, not the sheet.
    },
]

VECTOR_PARAMETERS = [
    {"name": "SimLocation", "value": (0.0, 0.0, 0.0, 0.0)},
    {"name": "FieldOrigin", "value": (0.0, 0.0, 0.0, 0.0)},
]


def parameter_collection():
    """Builds MPC_ARPG_Fluid, adding any parameter it is missing.

    ADDITIVE RATHER THAN AUTHORITATIVE. A parameter collection is one of the few
    assets where a designer genuinely might add something by hand -- a tint, a
    debug switch -- and rebuilding the arrays from this list would silently throw
    that away on the next run. Only names absent from the asset are appended.
    """
    path = "{}/MPC_ARPG_Fluid".format(NIAGARA_DIR)

    if unreal.EditorAssetLibrary.does_asset_exist(path):
        collection = unreal.EditorAssetLibrary.load_asset(path)
    else:
        unreal.EditorAssetLibrary.make_directory(NIAGARA_DIR)
        collection = asset_tools.create_asset(
            asset_name="MPC_ARPG_Fluid",
            package_path=NIAGARA_DIR,
            asset_class=unreal.MaterialParameterCollection,
            factory=unreal.MaterialParameterCollectionFactoryNew())

    if not collection:
        log("could not create MPC_ARPG_Fluid")
        return None

    scalars = list(collection.get_editor_property("scalar_parameters"))
    vectors = list(collection.get_editor_property("vector_parameters"))

    existing_scalars = set(str(e.get_editor_property("parameter_name")) for e in scalars)
    existing_vectors = set(str(e.get_editor_property("parameter_name")) for e in vectors)

    added = 0

    for spec in SCALAR_PARAMETERS:
        if spec["name"] in existing_scalars:
            continue

        entry = unreal.CollectionScalarParameter()
        entry.set_editor_property("parameter_name", spec["name"])
        entry.set_editor_property("default_value", spec["value"])
        scalars.append(entry)
        added += 1

    for spec in VECTOR_PARAMETERS:
        if spec["name"] in existing_vectors:
            continue

        r, g, b, a = spec["value"]
        entry = unreal.CollectionVectorParameter()
        entry.set_editor_property("parameter_name", spec["name"])
        entry.set_editor_property("default_value", unreal.LinearColor(r, g, b, a))
        vectors.append(entry)
        added += 1

    collection.set_editor_property("scalar_parameters", scalars)
    collection.set_editor_property("vector_parameters", vectors)

    save(path)
    log("MPC_ARPG_Fluid: {} parameters added, {} already present".format(
        added, len(SCALAR_PARAMETERS) + len(VECTOR_PARAMETERS) - added))

    return collection


def main():
    parameter_collection()
    fork_modules()
    custom_modules()

    system = fork_sheet()
    report_contract(system)

    log("---")
    log("STILL BY HAND, in this order:")
    log("  1. Fill in ARPG_SW_ReadFieldTexture and ARPG_SW_ConstrainToField.")
    log("  2. Repoint E_ARPG_FluidSheet's stack at /Game/ARPG/Fluids/Niagara/Modules,")
    log("     and swap the bottom-contour pair for ARPG_SW_ReadFieldTexture.")
    log("  3. Declare the user parameters this script just listed as missing.")
    log("  4. Run this script again -- it will say when the contract is complete.")
    log("Docs/FLUID_NIAGARA_AUTHORING.md is the long form.")

    with open(r"C:/Users/young/unreal/arpg/Saved/fluid_niagara_report.txt", "w") as handle:
        handle.write(chr(10).join(REPORT))


main()
