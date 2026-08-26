# Finishing the fluid sheet

Most of this is done. `Tools/generate_fluid_niagara_assets.py` forks the engine's
beta shallow-water content into `/Game`, creates the two modules we write
ourselves, wires the parameter collection, and — the useful part — **tells you
what is still missing** every time you run it.

What a script cannot do is fill in a Niagara graph. This was measured rather than
assumed: `unreal.NiagaraSystem` exposes thirty properties and not one of them is
an emitter, a module, or a parameter, and `unreal.NiagaraEmitter` exposes its
`UPROPERTY`s with no way into its graph. Nodes are out of reach from Python
permanently.

**But MCP is not Python.** The engine ships an editor-only plugin,
`NiagaraToolsets` — "a collection of tool calls allowing an AI assistant the
ability to interact with Niagara" — and it covers two of the three jobs below.
See *Doing jobs 2 and 3 over MCP*.

So there are three jobs left. One of them needs a person in the Niagara editor;
two of them do not.

---

## Run this first

```bash
"/c/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" arpg.uproject -ExecutePythonScript="Tools/generate_fluid_niagara_assets.py" -unattended -nopause -nosplash
```

It writes a report to `Saved/fluid_niagara_report.txt`. Run it again whenever you
want to know how far along you are — it overwrites nothing, so it is safe after
hand-editing.

### What it already did

| | |
|---|---|
| `/Game/ARPG/Fluids/Niagara/Modules/` | 21 engine modules, forked |
| `NS_ARPG_FluidSheet`, `E_ARPG_FluidSheet` | forked from `Grid2D_SW_Pool` and its emitter |
| `Modules/ARPG_SW_ReadFieldTexture` | created **empty** — job 1 |
| `Modules/ARPG_SW_ConstrainToField` | created **empty** — job 1 |
| `MPC_ARPG_Fluid` | five parameters, matching C++ |
| `Config/DefaultGame.ini` | both assets pointed at |

**Why forked at all.** `NiagaraFluids.uplugin` has `IsBetaVersion=true` and
`Content/Templates/` is demo content Epic reshapes between releases. A system
built directly on it changes under you on an engine upgrade, and the symptom is
water that looks different with no commit to blame. The cost is not getting
Epic's fixes for free; that is the right trade for a game.

**Two modules were deliberately not copied.** `Grid2D_Init_BottomCapture` and
`Grid2D_SW_ComputeBottomContour` grab the terrain with a scene capture — an extra
render pass, a frame of latency, and an answer that can disagree with the
simulation. We already have the truth on the CPU. `ARPG_SW_ReadFieldTexture`
replaces both. Do not add them back.

---

## Job 1 — the two module graphs

The only part nobody can drive for you. Both are small; the second is four nodes.

### The names you are working against

Read out of the engine's own assets rather than guessed. The shallow-water
emitter keeps two `Grid2D Collection` data interfaces, `WaterGrid` and
`VelocityGrid`, and the attributes on them are:

| Attribute | Meaning |
|---|---|
| `WaterHeight` | world Z of the surface |
| `WaterDepth` | how deep the water is |
| `BottomContour` | the floor the water sits on |
| `VelocityX`, `VelocityY` | flow |
| `IsCollider` | this cell is inside something solid |
| `OverlapVolume` | water displaced by a collider |

Module inputs are named `Input_<Thing>` and outputs `Out_<Thing>` — follow that,
because the stack UI keys off it.

---

### `ARPG_SW_ReadFieldTexture`

**Author it fresh. Do not duplicate `Grid2D_SW_ComputeBottomContour`.**

That advice was here and it was wrong. The engine module looks like a good
starting point from the outside -- its compiled asset mentions
`NiagaraDataInterfaceTexture`, `SampleTexture2D`, `Emitter.UseTexture`,
`TextureMult` and `TextureOffset` -- but those are parameter-map and
data-interface-registry residue in the serialised script, not nodes in its graph.
Open it and there is no texture sample. Reading strings out of a `.uasset` tells
you what a script was compiled *against*, not what it *does*.

What it actually carries is three bottom-contour sources you do not want -- a
landscape query, a scene capture, and an SDF collision query -- plus water-drop
initialisation and capture-position logic. Half a megabyte of asset, every branch
of which has to be found and deleted, to end up with something that samples one
texture.

The module you want is about eight nodes.

#### The contract it has to meet

Confirmed by inspecting the neighbouring modules rather than assumed:

| | |
|---|---|
| `Transient.BottomContour` | **written here, read by `Grid2D_SW_UpdateDepthGrid`.** This is the channel that matters -- it is what the solver treats as the floor |
| `Transient.IsCollider` | written here; `Grid2D_SW_UpdateDepthGrid` takes an `Input_IsCollider`, so check how that input is bound in the stack and point it at this |

`Grid2D_SW_UpdateVelocity` and `Grid2D_SW_Init` use no `Transient` values, so
those two are not part of this contract.

#### Module inputs to declare

| Input | Type | Bound in the stack to |
|---|---|---|
| `FieldTexture` | Texture | `User.FieldTexture` |
| `FieldOrigin` | Vector 2D | `User.FieldOrigin` |
| `FieldExtent` | float | `User.FieldExtent` |
| `UnitToWorld` | Matrix | same as the module it replaces |
| `Grid` | Grid2D Collection | `Emitter.WaterGrid` |

`UnitToWorld` is worth calling out: the engine module takes it as a module INPUT
rather than computing it, so the fiddly part of getting from a cell to a world
position is already handed to you by the stack. That was the only real argument
for duplicating, and it does not survive contact with the fact that it is one
input on a fresh module too.

#### The graph

**Script setup first.** New asset -> **Niagara Module Script**. In Script Details
set the **Module Usage Bitmask** to include **Particle Update** (and Simulation
Stage if the emitter runs one) -- a module whose bitmask does not cover the stage
you want simply never appears in the Add-Module list, with no error to explain
why. That is the single most common half-hour lost in this file.

**The spine.** Every module script is a parameter map flowing left to right:

```
Input node (Parameter Map, "InputMap")
    -> Map Get      reads the module's inputs
    -> ...work...
    -> Map Set      writes the outputs
    -> Output node (Module)
```

The Map Get is what DECLARES the module inputs -- add a pin named
`Module.FieldOrigin` and an input called FieldOrigin appears in the stack. There
is no separate declaration step.

**Node by node:**

| # | Node | Detail |
|---|---|---|
| 1 | **Map Get** | pins: `Module.Grid` (Grid2D Collection), `Module.FieldTexture` (Texture), `Module.FieldOrigin` (Vector 2D), `Module.FieldExtent` (float), `Module.UnitToWorld` (Matrix) |
| 2 | **ExecutionIndexToUnit** | drag off the `Module.Grid` pin. Outputs `UnitX`, `UnitY` -- 0..1 across the grid. This is the shortcut: it does ExecutionIndexToGridIndex and IndexToUnit in one |
| 3a | **Make Vector** | `Unit` is a **Vector2** on a Grid2D (Vector3 only on Grid3D). Build a Vector3 from `Unit.x`, `Unit.y`, `0` |
| 3b | **TransformPositionByMatrix** | `/Niagara/DynamicInputs/Math/TransformPositionByMatrix`. Pins are `Position` and `TransformationMatrix` -- feed the Vector3 and `Module.UnitToWorld`. Output is the world position |
| 3c | **Break Vector** | take X and Y into a Vector2 -> `WorldPos.xy` |
| 4 | **Subtract / Divide** | `UV = (WorldPos.xy - Module.FieldOrigin) / Module.FieldExtent` |
| 5 | **SampleTexture2D** | drag off `Module.FieldTexture`. Signature is `(UV: Vector2, MipLevel: float) -> Value: Vector4`. Pass **MipLevel 0** |
| 6 | **Break Vector4** | take `Value` apart -> R, G, B, A |
| 7 | **Map Set** | `Transient.BottomContour` = **B** |
| 8 | **Map Set** | `Transient.IsCollider` = `A > 0.5` (a Greater-Than into a bool) |

Eight nodes plus the spine.

`ExecutionIndexToUnit` is worth knowing about -- the engine modules mostly use
`ExecutionIndexToGridIndex` followed by `IndexToUnit`, which is the same thing in
two nodes.

**Not the `Transform Position` node.** That one converts between engine SPACES --
its pins are Source Space and Destination Space dropdowns, World / Simulation /
Local -- and it has nowhere to put a matrix. `UnitToWorld` is an arbitrary
transform the emitter builds (see the engine's `Grid2D_CreateUnitToWorldTransform`
module), so it needs the matrix node.

**Check what the original bound `UnitToWorld` to.** Your module takes the same
input for the same reason the one it replaces did, so in the stack it wants the
same binding -- look at `Grid2D_SW_ComputeBottomContour`'s `UnitToWorld` pin in
the emitter and copy it.

**Subtraction is not commutative and the pins are not labelled.** `WorldPos.xy`
goes in the FIRST input of the subtract and `FieldOrigin` in the second. Backwards
gives a UV that is mirrored through the origin, which reads as the water being
somewhere else entirely rather than as an obvious error.

> **If the water comes out mirrored in Y, flip V.** `SampleTexture2D`'s own
> description says "the UV origin (0,0) is in the upper left hand corner of the
> image", while `FARPGFluidField::SampleWindow` writes buffer row 0 as the
> LOWEST world Y. Those agree as written -- V and world Y both increase together
> -- but it is exactly the kind of convention that is off by a flip in practice,
> and the symptom is water that is correct in shape and reflected about the
> middle of the window. `V = 1 - V` if so.



1. `Emitter.ExecutionIndex` -> **ExecutionIndexToGridIndex** on `Grid` -> IndexX, IndexY
2. **IndexToUnit** -> unit position, 0..1 across the grid
3. **TransformPosition** by `UnitToWorld` -> `WorldPos`
4. `UV = (WorldPos.xy - FieldOrigin) / FieldExtent`
5. **SampleTexture2D**(`FieldTexture`, `UV`) -> RGBA
6. `Transient.BottomContour = B`
7. `Transient.IsCollider = A > 0.5`

The texture is clamped and bilinear, so there is no wrapping or filtering to add.

#### What the channels mean

Written by `FARPGFluidField::SampleWindow`. `PF_FloatRGBA`, 128 square.

| | |
|---|---|
| **R** | depth of fluid, cm. **Zero inside a blocked cell**, whatever interpolation would say |
| **G** | the floor, world Z |
| **B** | top of anything solid standing here, else the floor |
| **A** | `1` where fluid may not enter, `0` where it may |

**B, not G, is the bottom contour.** `max(floor, solid)` is what makes a
shallow-water solver treat a wall as floor that is higher up, so water parts
around it with nothing in the graph knowing what a wall is. Writing G instead
gives you water that flows straight through walls and looks almost right.

`ARPG.World.FluidField.TheWindowHandedToTheGpuSaysWhatTheFieldSays` asserts every
texel against the field. If you change this packing, change that test.

> **Why the alpha channel is one flag and not two.** It briefly packed `1` for
> blocked and `2` for roofed, which is tidy and unusable. A floating floe is
> roofed and NOT blocked, so it read as 2 -- and the obvious test on this end,
> `A >= 1`, turned every floe into a dam. That is precisely the distinction the
> two flags exist to keep apart, undone by packing them into one number. The
> channel is also bilinearly filtered, so even the correct bit test lands on 1.5
> somewhere along every boundary.
>
> Roofed is not lost. B carries the top of whatever stands here and G carries the
> floor, so `B > G` asks the same question. Blocked cannot be derived that way --
> a floe raises B without blocking anything -- which is why it is the one that
> keeps the channel.

> **Verify one thing:** whether `Transient.BottomContour` is world Z or relative
> to the emitter. My C++ writes G and B as **world Z**. If the stack works in
> relative space, subtract the emitter origin here -- one node, and a hard bug to
> find later, because the water looks fine and sits at the wrong height.

### The constraint — fold it into `ARPG_SW_ReadFieldTexture`

**There is no second module.** The generator creates an empty
`ARPG_SW_ConstrainToField` because that was the original plan; delete it. The
constraint belongs in the module that already samples the texture, for three
reasons:

1. **It needs the same sample.** Splitting means either sampling the texture
   twice or shuttling R and G through `Transient` to reach the second module.
2. **It removes the stage-ordering question entirely.** A separate module has to
   be placed relative to the solver, and Grid2D reads differ depending on which
   side you land on.
3. **Before the solver is the better place anyway.** Constraining first means the
   solver evolves detail *on top of* corrected truth. Constraining after means
   the ripples it just made get partly flattened every frame -- the sheet fights
   the constraint instead of riding it.

`ARPG_SW_ReadFieldTexture` replaces `Grid2D_SW_ComputeBottomContour`, which is the
per-frame module (`Grid2D_Init_BottomCapture` is the init one, and we do not use
it). So it already runs every frame, before the depth update. That is exactly
where the constraint wants to be.

#### Constrain DEPTH, not height

`Grid2D_SW_UpdateDepthGrid` **writes `WaterHeight`** and takes `Input_WaterDepth`
-- verified by reading the asset. So it derives height from depth and bottom
contour every frame. Constraining `WaterHeight` from a module that runs BEFORE it
is writing a value that is about to be recomputed and thrown away: the constraint
appears to do nothing, and nothing anywhere reports an error.

Constrain the quantity the solver actually carries:

```
Alpha      = saturate(ConstrainRate * Engine.DeltaTime)
WaterDepth = Lerp(WaterDepth, R, Alpha)
```

Height then follows on its own, from our `BottomContour` and the constrained
depth, which is the whole point of feeding B into the bottom contour.

**And it is unit-safe.** `R` is a depth in centimetres. Constraining height would
have meant matching G against whatever space `WaterHeight` is in -- the world-Z
question flagged above -- and depth has no such ambiguity in either convention.
It also drops the add node: the target is R on its own, not G + R.

#### What to add

Two more Map Get pins: **`Module.ConstrainRate`** (float, bound to
`User.ConstrainRate`) and **`Engine.DeltaTime`**.

#### `WaterDepth` is a grid attribute, not a parameter

This is the part that catches people. `Transient.BottomContour` is a value in the
parameter map, so it is written with a **Map Set** pin. `WaterDepth` is an
attribute *inside the Grid2D Collection*, so it is reached with **function calls
on `Module.Grid`** -- it never appears on Map Get or Map Set at all.

    read    GetPreviousFloatValue   -> Lerp A
    write   SetFloatValue           <- Lerp output

`GetPreviousFloatValue` because this module runs before the depth update: the
current buffer has not been written yet this frame, and the previous one holds
what the last frame's solver left.

The attribute is addressed by index rather than by name, and the exact pattern is
fiddly enough to be worth copying rather than reconstructing:

> Open `Grid2D_SW_UpdateDepthGrid` and mirror how it reads and writes
> `WaterDepth` on `Emitter.WaterGrid`.

Concretely, three nodes off the `Module.Grid` pin:

| Node | Attribute | Pins |
|---|---|---|
| **Execution Index to Grid Index** | -- | `Grid` in; `IndexX`, `IndexY` out |
| **Get Previous Float Value** | `WaterDepth` | `Grid`, `IndexX`, `IndexY`; `Value` out -> Lerp **A** |
| **Set Float Value** | `WaterDepth` | `Grid`, `IndexX`, `IndexY`, `Value` <- Lerp output |

`IndexX` and `IndexY` default to literal `0` on those nodes, which reads and
writes cell (0,0) for every thread in the grid -- a uniform sheet at whatever
happens to be in the corner. They must be wired.

**`Set Float Value` requires an execution pin.** Its signature carries
`bRequiresExecPin = true`, unlike every other node in this graph: Niagara
dataflow has no inherent ordering, and a write has to be told when it happens.
Wire it or the write is not merely unordered, it does not compile.

Writing a grid attribute is a FUNCTION CALL on the data interface, not a Map Set
pin -- which is why `Module.Grid` appears in the Map Get input list and never on
the output side. Map Set is for parameter-map values like
`Transient.BottomContour`. The grid is an object you call methods on.

#### Tuning

`ConstrainRate` is set from C++ at **6**. Between constraints the sheet is free:
it ripples, breaks and foams at 4cm, which the 40cm field cannot represent. The
constraint drags it back to where the simulation says the water actually is.

- Too high: the sheet is a blurry copy of the field, with no life in it.
- Too low: the water you see is not the water you swim in.

Change the default in `ARPGFluidPresentationSubsystem.cpp` if you retune it, or
the next person gets your number overwritten on the first frame.

---

### When you are done

Run the generator again:

```bash
"/c/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" arpg.uproject -ExecutePythonScript="Tools/generate_fluid_niagara_assets.py" -unattended -nopause -nosplash
```

It reports to `Saved/fluid_niagara_report.txt`. Jobs 2 and 3 can then be driven
over MCP — see below.

---

## Job 2 — repoint the emitter

`E_ARPG_FluidSheet` still calls the **engine's** modules, because a duplicate
keeps the references its original had. Point its stack at
`/Game/ARPG/Fluids/Niagara/Modules/` instead, and swap the bottom-contour pair
for `ARPG_SW_ReadFieldTexture`.

*(The one Python tool that rewrites references, `consolidate_assets`, deletes the
asset it redirects away from — which here would mean deleting content inside the
engine install. So not Python — but see below, because MCP can do this one.)*

---

## Job 3 — declare the user parameters

Exact names and types. C++ sets every one; the script lists which are still
missing.

| Name | Type | Set from |
|---|---|---|
| `WorldGridSize` | Vector 2D | `WindowSize`, both axes |
| `ResolutionMaxAxis` | int | `Resolution` (512) |
| `NormalRT` | Texture Render Target 2D | created per sheet |
| `VelocityRT` | Texture Render Target 2D | created per sheet |
| `FieldTexture` | Texture 2D | uploaded every field step |
| `FieldOrigin` | Vector 2D | window's minimum corner, snapped to a texel |
| `FieldExtent` | float | `WindowSize` |
| `ConstrainRate` | float | 6 |
| `SurfaceMaterial` | Material | the fluid definition's `SurfaceMaterial` |

`SurfaceMaterial` is the **only** thing that differs between the water sheet and
the lava sheet — bind it on the emitter's mesh renderer so one system draws both.

Nothing checks these names at build time. A misspelling is a silent no-op at
runtime and the symptom is a sheet that draws nothing with no error anywhere,
which is why the script checks them instead.

### Six of the nine are already there

`WorldGridSize` and `ResolutionMaxAxis` came with the fork. `FieldOrigin`,
`FieldExtent`, `ConstrainRate` and `SurfaceMaterial` were added over MCP with
`AddUserVariables`, and the system has been saved.

**The three render-target and texture ones have to be added by hand.** Create
them in the Niagara System editor: select the **User Parameters** node at the top
of the System Overview (or open the **Parameters** panel and find the *User
Exposed* section), press **+**, and pick the type. Name them without the `User.`
prefix -- that panel adds it.

| Name | Type to pick |
|---|---|
| `FieldTexture` | Texture |
| `NormalRT` | Render Target 2D |
| `VelocityRT` | Render Target 2D |

Leave them **empty**. C++ assigns the real objects every frame, and an asset set
here would only be a default that never survives the first tick. Save the system
afterwards.

The reason this cannot be scripted is worth writing down so nobody wastes an
afternoon on it:
`AddUserVariables` requires *both* `dataInterfaceClass` **and** `dataInterface`
for a data-interface variable — a reference to an actual instance. The instance
only comes into existence when the variable is created. There is no way out of
that from the tool: passing the class default object is accepted and then
silently dropped, and passing nothing gives

> *"type 'NiagaraDataInterfaceTexture' has no fixed size (object or interface
> type) and no default value was provided."*
>
> and naming a subobject that does not exist yet gives
>
> *"...:NiagaraDataInterfaceTexture_0 is not valid NiagaraDataInterface for
> property..."*

Object references are fine — that is why `SurfaceMaterial` went in, pointing at a
material that already exists.

So in the **User Parameters** panel of `NS_ARPG_FluidSheet`, add:

| Name | Type |
|---|---|
| `NormalRT` | Texture Render Target 2D |
| `VelocityRT` | Texture Render Target 2D |
| `FieldTexture` | Texture |

Three drags. Everything else about job 3 is done.

---

## Doing jobs 2 and 3 over MCP

`NiagaraToolsets` is now enabled in `arpg.uproject`. It is editor-only and
experimental, and it needs an **editor restart** to load — a plugin cannot be
brought in by Live Coding.

Once the editor is back, the project's MCP server on `localhost:8000` exposes it
alongside the toolsets already there (automation tests, GAS, config, gameplay
tags, editor app, logs, live coding). The relevant calls:

> **Job 2 turned out NOT to be drivable, and this is the evidence so nobody
> spends another hour on it.** Every stack call against this emitter answers
> *"Script 'ParticleUpdateScript' not found in stack reference"*, for all four
> script names it knows. The same calls work fine on a CPU emitter in this
> project (`NS_JumpPad` / `Glow_Base` returns four modules), and
> `GetEmitterData` resolves our emitter's properties happily -- so the reference
> is good and it is specifically the STACK that is missing.
>
> `GetSystemCompileState` says why: this emitter compiles to **thirteen
> `ParticleSimulationStageScript`s** plus a `ParticleGPUComputeScript`. The
> shallow-water solver lives entirely in simulation stages, and the toolset's
> vocabulary has only EmitterSpawn, EmitterUpdate, ParticleSpawn and
> ParticleUpdate. There is no `scriptName` that reaches a simulation stage.
>
> So job 2 is by hand. The rest of the table still holds.

| Job | Calls |
|---|---|
| Read the stack first | `GetEmitterTopology`, `GetScriptStackTopology`, `GetModuleTopology` -- **simulation stages excepted, see above** |
| **Job 2** — repoint | by hand |
| **Job 3** — parameters | `AddUserVariables` (six of nine; see above) |
| Checking the result | `GetStackIssues`, `GetSystemCompileState` |
| Getting it on disk | `editor_toolset...AssetTools.save_assets` |

**Job 1 is not reachable, and this was checked rather than assumed.** All five
Niagara toolsets were enumerated -- fifty-six tools between them -- and every one
of them operates on a SYSTEM: emitters, stacks, modules-in-a-stack, renderers,
user variables. `NiagaraToolset_Assets` has exactly three, all discovery:
`FindNiagaraScripts`, `GetAssetDiscoveryInfo`, `GetNiagaraScriptDigest`.

`AddModule` places an *existing* module script into a stack. Nothing anywhere
writes a module script's internal graph.

So the split is: a person writes the two module graphs and adds three
data-interface parameters, and everything else can be driven.

---

## Job 4 — the renderer

Done over MCP on 2026-08-24; written down because it is invisible from the asset
list and it is what a forked pool template gets wrong by default.

`E_ARPG_FluidSheet` has exactly one renderer, a `NiagaraMeshRendererProperties`.
Two of its properties are ours and neither survives a re-fork:

| Property | Must be | Was, out of the template |
|---|---|---|
| `OverrideMaterials[0].userParamBinding` | `User.SurfaceMaterial` | `None` |
| `OverrideMaterials[0].explicitMat` | `M_ARPG_Water` | `DebugWaterMaterial` |
| `Meshes[0].mesh` | `SM_ARPG_FluidSheet` | `/Engine/BasicShapes/Plane` |
| `Meshes[0].meshParameterBinding` | `None` | `Emitter.GridMesh` |

**The user parameter binding is the one that matters.** C++ builds a dynamic
material instance per element and sets `FieldTexture`, `NormalRT` and
`VelocityRT` on it, then hands it over as `User.SurfaceMaterial` -- so a renderer
pointed at the base material draws with the default black field texture and is
invisible, and a renderer pointed at Epic's `DebugWaterMaterial` paints the whole
2048cm domain in a depth ramp. That ramp is what "a flat blue plane that follows
the player" looks like, with a green lobe where the ramp turns over. `explicitMat`
is only the fallback the Niagara editor preview uses, where no user parameter is
set; keeping it on `M_ARPG_Water` makes the preview honest.

**The mesh has to be subdivided**, and `/Engine/BasicShapes/Plane` has four
vertices. `M_ARPG_Water` puts the surface at the field's own height by offsetting
each vertex onto `FieldTexture.G + FieldTexture.R`; applied to four corners that
is not a water surface, it is one quad tilted by whatever the field said beneath
those corners. `Tools/generate_fluid_sheet_mesh.py` builds `SM_ARPG_FluidSheet`
-- 100cm square so it is a drop-in for the engine plane, 128 steps so there is
one vertex per field texel, 32,258 triangles.

The mesh binding is Epic's hook for letting the pool template's own stack choose
a mesh. We do not want it choosing; clearing it to `None` makes the explicit
asset win.

**Do not "fix" the height in C++ again.** `UARPGFluidPresentationSubsystem` sets
the component's Z to the bed under the viewer, and that is now only a base plane
-- close enough to keep the offsets small and the bounds honest. It went through
Z=0 (water at the bottom of the level) and the viewer's own Z (water bobbing when
they jumped) before the material took the job over. The material is where it
belongs, because only the material gets a value per texel.

## The material

`MPC_ARPG_Fluid` carries what a material cannot work out for itself:

| Parameter | Meaning |
|---|---|
| `SimLocation` | window centre — **last frame's**, see below |
| `FluidSimSize` | window width, cm |
| `FluidSimResolution` | texels across the sheet |
| `FieldOrigin` | minimum corner of the field picture |
| `FieldExtent` | its width, cm |

`SimLocation` is deliberately a frame behind. Niagara's GPU work runs after the
game thread has moved the component, so the grid the material samples this frame
was simulated around the previous centre. Publishing the current one makes the
whole surface slide under the player when they run.

The first three names are Epic's own defaults from `UShallowWaterSettings`, so
the material functions under `NiagaraFluids/Content/Materials/ShallowWater` drop
in unmodified.

**Note:** `r.Lumen.TraceMeshSDFs=0` in `Config/DefaultEngine.ini`, so reflections
will not come from Lumen SDF tracing. Plan for SSR or hardware ray tracing, or
accept flat reflections.

---

## Turning it on

PIE, then `ARPG.Fluid.Presentation 1`. Both assets are already wired in
`Config/DefaultGame.ini`.

Until job 2 is done the sheet behaves like the Epic template it was forked from —
it will not read the field and will not look like this game's water. That is
harmless: presentation is off by default, and every body draws a plain slab of
its own outline while it is. The slab hides itself when the sheet is on and comes
back when it is off, so the two are never both visible and switching off never
leaves the water invisible.

### Checks

```
stat GPU
fx.Niagara.Debug.Hud Enabled=1
fx.DumpNiagaraWorldManager
```

- At most two sheets alive (`MaxSheets`), and none when no water is near.
- Budget: under 1.5 ms GPU.
- `ARPG.Fluid.Presentation 0` then `1` — **gameplay must be identical either
  way.** `ARPG.World.Fluid.Surface.DrawingTheWaterChangesNothingAboutIt` asserts
  it headlessly; check it in PIE too whenever this file changes.
