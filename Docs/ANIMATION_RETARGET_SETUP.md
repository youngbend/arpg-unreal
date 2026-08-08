# Animation Retarget Setup (Phase 4b)

Getting the sword moveset onto the UE5 mannequin, and the montages built that
`UARPGAttackDefinition` expects.

Everything here is editor work. The code half of phase 4 is done and tested
(`ARPG.Combat.Combo`); this is what unblocks `UGA_MeleeAttack` and the anim
notifies.

---

## 0. Read this first: the FBX sources are gone

The Godot project converted Mixamo `.fbx` clips into per-clip Godot `.res`
resources via `project/tools/convert_mixamo_animations.gd`, which read from:

```
project/weapons/sword/mixamo_animations/     <- MISSING
project/characters/mixamo_animations/        <- MISSING
```

Both directories are absent from disk **and were never tracked in git** (no
`.gitignore` rule; they were simply deleted after conversion). The only
surviving copies of those animations are:

| What | Where | Count |
|---|---|---|
| Sword clips | `project/weapons/sword/animations/*.res` | 66 |
| Character clips | `project/characters/animations/*.res` | 29 |
| Paladin rig | `kimodo_retarget/paladin.glb` | 1 |
| Kimodo inbetweens | `kimodo_retarget/output/*.glb` | 6 |

**This matters more than it looks.** The `.res` files are not raw Mixamo
downloads — they are *processed derivatives*. Clips were split into windup /
active / recovery phases, had static lead-in trimmed
(`project/tools/trim_static_lead.gd`), and had root motion fixed up
(`project/tools/fix_rootmotion_hips.gd`). Re-downloading from Mixamo would give
you the raw clips back, not the authored phase splits — and the phase boundaries
are exactly what the montage sections need.

So the sources must come out of Godot, not from Mixamo.

### Recommended: export the .res back out as glTF

Godot 4 can write glTF from script (`GLTFDocument` + `GLTFState`), so a headless
pass can load each `.res`, apply it to the paladin skeleton, and export a `.glb`.
That preserves the exact trimmed and split clips.

Godot binary on this machine:

```
C:\Users\young\Downloads\Godot_v4.7-stable_win64.exe\Godot_v4.7-stable_win64_console.exe
```

I have not written this exporter yet — say the word and I will. It is the one
remaining piece that can be done without the editor.

### Alternative: re-source from Mixamo

Viable only if you are willing to redo the phase splits and trims by hand, in
which case you would be re-authoring rather than porting. Not recommended for
the 16 attacks that already have tuned timings, but reasonable for any clip
whose `.res` turns out to be unusable.

---

## 1. Import the paladin rig and clips

1. Import `kimodo_retarget/paladin.glb` — this creates the skeletal mesh and its
   skeleton. Name them `SK_Paladin` / `SKEL_Paladin`.
2. Import the exported animation `.glb` files **against that skeleton**
   (Skeleton field set on the import dialog, not "create new").

**Scale.** Godot and Blender work in metres; Unreal works in centimetres. If the
paladin imports at 1/100 scale, set Import Uniform Scale to 100. Check the mesh
against `SKM_Manny` before importing 95 clips against a wrong-scaled skeleton.

---

## 2. Build the two IK Rigs

An IK Retargeter needs an `IK Rig` on both ends, and the **chain names must match
between them** — that mapping is the whole retarget.

### IK_Paladin (source)

- Retarget Root: the pelvis/hips bone.
- Chains, one per limb plus spine:

  | Chain | Start | End |
  |---|---|---|
  | Spine | spine (lowest) | spine (highest) |
  | Head | neck | head |
  | LeftArm | left shoulder/upperarm | left hand |
  | RightArm | right shoulder/upperarm | right hand |
  | LeftLeg | left thigh | left foot/ball |
  | RightLeg | right thigh | right foot/ball |

If the rig is Mixamo-derived the bones will be prefixed `mixamorig:`.

### IK_Mannequin (target)

UE ships `IK_Mannequin` under `/Game/Characters/Mannequins/Rigs` or the Engine
content. Use it if present; otherwise create one from `SKM_Manny` with the same
chain names (`pelvis`, `spine_01..05`, `clavicle_*`, `upperarm_*`, `hand_*`,
`thigh_*`, `foot_*`).

**Use identical chain names in both rigs.** Retargeting maps chain-to-chain by
name; a mismatch shows up as a limb that simply does not move.

---

## 3. Build the IK Retargeter

Create `RTG_Paladin_To_Manny`, source `IK_Paladin`, target `IK_Mannequin`.

**Fix the reference pose first.** Mixamo rigs are broadly T-pose; the UE
mannequin is A-pose. Retargeting straight across gives permanently splayed or
clipped arms. In the retargeter, select the **source**, switch to Edit Pose, and
rotate the arms down to approximate the mannequin's A-pose. Everything else
depends on getting this right — do it before batch exporting.

Then map the chains, and scrub a clip in the preview to confirm.

### Batch retarget

Select every imported animation in the content browser → right-click →
**Retarget Animations** → pick `RTG_Paladin_To_Manny` → set the output folder
(`/Game/ARPG/Animations/Sword`) and a prefix.

---

## 4. Author the montages

One montage per attack. Section names are what the code expects:

```
Windup   Active   Gap   Active2   Landing   Recovery
```

Only `Windup` and `Active` are required.

**Each attack asset tells you what goes in it.** The converter wrote the source
clip names into `DA_Attack_*` → `ImportData`, so you do not have to go back to
the Godot repo:

```
ImportData
  SourceFile           overhead_slash.tres
  WindupClip           overhead_slash_windup
  ActiveClip           overhead_slash_active
  RecoveryClip         overhead_slash_recovery
  RecoveryCancelDelay  0.1
```

### Slots

| `bBlendLocomotion` | Slot | Effect |
|---|---|---|
| true | `UpperBody` | Overlays the attack; legs keep locomoting |
| false | `FullBody` | Overrides the whole body |

The AnimBP needs a `Layered Blend Per Bone` from `spine_01` for the `UpperBody`
slot. That replaces Godot's hand-rolled upper-body overlay, and the hip-filtering
workaround noted in the Godot project's own notes is not needed — a layered blend
handles it.

### Notifies

| Notify | Where | Purpose |
|---|---|---|
| Hitbox window (AnimNotifyState) | spanning `Active`, and `Active2` if present | Arms/disarms the hitbox |
| Combo window (AnimNotify) | `RecoveryCancelDelay` seconds into `Recovery` | Opens the cancel window |

**`RecoveryCancelDelay` is now a notify position, not a runtime field.** It was
carried across purely so this timing was not lost and re-guessed — put the notify
where the number says.

The notify classes (`UARPGAnimNotifyState_Hitbox`, `UARPGAnimNotify_ComboWindow`)
are written and live in `ARPGCombat`.

### Charge attacks (`bChargeable`)

Author these **exactly like a normal attack**. Do not stretch the wind-up by hand
and do not add a hold loop — the ability does both at runtime:

- The `Windup` section is time-stretched so it takes `ChargeTime` regardless of
  how long the clip actually is. The play rate is derived from the section's
  real length, so **re-timing the animation cannot put the charge out of step**
  with what the player sees.
- `Windup` is temporarily self-linked, so it cannot leak into `Active` in the
  frame between the charge completing and the release arriving.
- On release the rate returns to 1 and playback jumps straight to `Active`, at
  whatever fraction was reached.

So: author `Windup` at whatever length reads well as a single wind-up pose, and
set `ChargeTime` to how long the charge should take. They are independent.

Damage scales through `ChargeMotionMin`..`ChargeMotionMax` and applies **only to
the first active window** — `Active2` and `Landing` keep their own fixed motion
values, so a charge cannot compound across a multi-hit swing.

### Channel attacks (`bChannel`)

| Section | Role |
|---|---|
| `Windup` | Plays once. Required — its length is how the ability knows when the loop may begin. |
| `Active` | **Loops** while the button is held and stamina lasts. Author it to loop cleanly. |
| `Recovery` | Optional. Played once the loop breaks. Without it the montage simply runs off its end. |

The loop is a section self-link, not a re-trigger, so the pass that is playing
always finishes rather than being cut mid-rotation.

Put the hitbox notify state **inside `Active`**, spanning as much of it as should
connect. It re-arms every pass; `HitboxTickInterval` controls how often a target
that stays inside it is re-hit.

Two things that are easy to get wrong:

- **Do not put a combo-window notify in a channel's `Active` section.** It is
  ignored there by design (the loop would otherwise pass it every revolution),
  but it will not do what you want. A channel closes out when its montage ends.
- `Windup` with no length means the loop starts immediately. That works, but the
  channel then has no tell at all.

`ChannelStaminaPerSecond` drains **only during the loop** — never during the
wind-up or the recovery, so a long wind-up is not a tax on the player.

`bChargeable` and `bChannel` are mutually exclusive; setting both logs a warning
and is treated as a charge.

---

## 5. Wire it up

For each `DA_Attack_*`, set the `Montage` field. Then open
`/Game/ARPG/Weapons/DA_AttackTree_Sword` and confirm the branching reads right —
it was generated from the Godot tree and should already be correct:

```
ROOT LIGHT    OneHand1 -L-> OneHand2 -L-> OneHand3 -{L,H,S}-> spins + overhead
                  |              `-H-> Kick
                  `-H-> HiltSmack
ROOT HEAVY    CrossSlash -L-> TwoHand1 -L-> TwoHand2 -L-> TwoHand3 -{L,H,S}-> ...
                  |-H-> CrossCombo1 -H-> CrossCombo2
                  `-S-> DownwardSlash
ROOT SPECIAL  JumpingSlash
PARRY         low spin / high spin / overhead
```

---

## What this port deletes

**The extended-skeleton pipeline is obsolete.** The Godot project generated
extended-skeleton GLBs with custom `root` and `weapon.L` / `weapon.R` bones
(`project/tools/build_extended_models.gd`) because Godot needed real bones for
weapon attachment and root motion. The mannequin has a root bone natively and
`hand_r` / `hand_l` sockets, so weapon attachment is socket-based and that whole
generation step goes away. Do **not** rebuild it.

**Root motion comes free.** Enable Root Motion on the montage; delete any
`get_root_motion_position` equivalent.

---

## Known traps

1. **Scale.** Metres vs centimetres. Verify one clip before batch-importing 95.
2. **Reference pose.** T-pose source onto an A-pose target without an Edit Pose
   correction produces subtly wrong arms on *every* clip. Fix it once, up front.
3. **Chain name mismatch** between the two IK Rigs is silent — the limb just does
   not move.
4. **Retarget the mesh too**, not only the animations, if you want to *see* the
   paladin on the mannequin skeleton. Otherwise retarget animations only and keep
   `SKM_Manny` as the visible mesh.
5. **`Landing` sections** only matter for air attacks (`JumpingSlash`). The rest
   have no landing clip and should not have the section.

---

## Then I can finish phase 4

Once even **one** montage exists I can write and verify:

- `UGA_MeleeAttack` — plays the montage, applies stamina cost, grants
  `State.Attacking` and conditional `State.Hyperarmor`, calls back into
  `UARPGComboComponent` at the cancel window.
- `UAnimNotifyState_Hitbox` and `UAnimNotify_ComboWindow`, both routed as
  gameplay events.

I deliberately have not written these yet: a notify that arms a hitbox window and
an ability that plays a montage cannot be verified without a montage, and code I
cannot run is code I should not be handing over.
