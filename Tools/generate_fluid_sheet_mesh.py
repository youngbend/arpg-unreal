"""Builds the grid the Niagara fluid sheet is drawn on.

    "<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" <path>/arpg.uproject \
        -ExecutePythonScript="<path>/Tools/generate_fluid_sheet_mesh.py" \
        -unattended -nopause -nosplash

WHY NOT /Engine/BasicShapes/Plane, which is what the sheet's mesh renderer was
using: it has four vertices. M_ARPG_Water puts the water surface at the field's
own height by offsetting each vertex onto FieldTexture.G + FieldTexture.R, and
an offset applied to four corners is not a water surface -- it is one enormous
quad tilted by whatever the field happened to say beneath those four points.
A flat pool material never noticed, because a pool is flat.

SIZED AND ORIENTED AS A DROP-IN. 100cm square, centred, facing +Z, exactly like
the engine plane, so swapping it on the renderer needs no other change.

SUBDIVIDED TO MATCH THE FIELD TEXTURE, not the Niagara grid. The texture is 128
across the window, so 128 steps puts one vertex on each texel: finer buys
nothing but interpolation between samples that were never distinct, and coarser
throws away height the CPU already computed.
"""

import unreal

MESH_DIR = "/Game/ARPG/Fluids/Niagara"
MESH_NAME = "SM_ARPG_FluidSheet"

SIZE = 100.0
STEPS = 128


def log(message):
    unreal.log("[ARPG sheet mesh] {}".format(message))


def build():
    path = "{}/{}".format(MESH_DIR, MESH_NAME)

    mesh = unreal.DynamicMesh()

    options = unreal.GeometryScriptPrimitiveOptions()
    transform = unreal.Transform()

    mesh = unreal.GeometryScript_Primitives.append_rectangle_xy(
        mesh, options, transform, SIZE, SIZE, STEPS, STEPS)

    triangles = mesh.get_triangle_count()
    log("built {} triangles at {} steps".format(triangles, STEPS))

    if triangles < STEPS * STEPS:
        log("REFUSING to save: {} triangles is not a {}x{} grid".format(
            triangles, STEPS, STEPS))
        return

    asset_options = unreal.GeometryScriptCreateNewStaticMeshAssetOptions()
    asset_options.set_editor_property("enable_recompute_normals", True)
    asset_options.set_editor_property("enable_recompute_tangents", True)

    unreal.GeometryScript_NewAssetUtils.create_new_static_mesh_asset_from_mesh(
        mesh, path, asset_options)

    static_mesh = unreal.EditorAssetLibrary.load_asset(path)

    if not static_mesh:
        log("create_new_static_mesh_asset_from_mesh produced nothing at {}".format(path))
        return

    if unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
        log("{}: {} triangles, {}cm square".format(MESH_NAME, triangles, SIZE))
    else:
        log("SAVE REFUSED for {} -- is the editor open?".format(path))


build()
