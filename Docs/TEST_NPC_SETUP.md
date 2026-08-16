# Test NPC setup

A Manny-bodied enemy that perceives, closes, swings, parries, flees and heals —
the fixture that makes phase 8 observable in a running game rather than only in
automation tests.

## What exists in code

| Thing | Where |
|---|---|
| The body every NPC uses | `AARPGNPCCharacter` (`Source/arpg/`) |
| The mind | `AARPGAIController` — perception + `UStateTreeAIComponent` |
| The archetype | `UARPGNPCDefinition` — stats, weapon, mesh, tree, leash |
| Tasks / conditions | `Source/ARPGAI/Public/ARPGStateTree*.h` |

There is deliberately **no subclass per enemy type**. A new enemy is a
`DA_NPC_*` asset plus, if it needs a different body, a Blueprint over
`AARPGNPCCharacter`.

## Step 1 — generate the assets

```
"<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" <path>/arpg.uproject \
    -ExecutePythonScript="<path>/Tools/generate_test_npc.py" \
    -unattended -nopause -nosplash
```

Creates:

- `/Game/ARPG/Weapons/sword/DA_Weapon_Sword` — wraps the existing
  `DA_AttackTree_Sword`. The NPC needs a weapon *definition*, not just a tree:
  `UARPGNPCComponent` equips definitions, and the AI's attack-range condition
  reads `Reach` off one.
- `/Game/ARPG/NPCs/DA_NPC_TestDummy` — the archetype.
- `/Game/ARPG/NPCs/BP_NPC_Manny` — `AARPGNPCCharacter` + `SKM_Manny_Simple` +
  `ABP_Unarmed`.

If the script reports it could not reach the Blueprint's mesh component, open
`BP_NPC_Manny` and set the mesh and anim class by hand — the log names exactly
which fields it skipped.

## Step 2 — the StateTree

`Tools/generate_npc_statetree.py` **probes** whether StateTree authoring is
reachable from Python on your engine build and prints what it finds. It does not
write a partial asset. Run it; if it says no, author the tree by hand with the
recipe below — it is roughly ten minutes.

### The structure

Selection is top-down, first match wins, so **the order is the priority**.

```
Root                                        evaluator: ARPG Perception
├─ Return Home    [ARPG Is Leashed]         → Move To (HomeLocation)
├─ Recover        [ARPG Health Below 0.35]  → ARPG Create Space
│                                           → ARPG Use Consumable
├─ Combat         [bHasTarget]
│   ├─ Parry                                → ARPG Attempt Parry
│   ├─ Strike     [In Attack Range]
│   │             [Facing Target]           → ARPG Attack (Light)
│   ├─ Close      [not In Attack Range]     → ARPG Face Target
│   │                                       → Move To (Target)
│   └─ Square Up                            → ARPG Face Target
├─ Investigate    [bHasInvestigateLocation] → Move To (InvestigateLocation)
└─ Idle                                     → Delay 1s
```

**Why Parry sits above Strike.** It fails on entry within one tick when the
target is not mid-swing, so putting it first costs nothing — and it means a
swing coming in always outranks one going out.

**Why Leash and Recover sit above Combat.** Both are answers to "stop fighting".
Checked after the combat branch had already been selected, they would only take
effect once the current attack finished — which is exactly the interruption the
StateTree migration was for.

### Authoring it

1. Content Browser → `ARPG/NPCs` → right click → **Artificial Intelligence →
   State Tree**. Schema: **StateTree AI Component**. Name it `ST_NPC_Melee`.
2. On the **root**, add the evaluator **ARPG Perception**. Do this first —
   everything else binds to its outputs, and nothing works until it exists.
3. Add the states in the order above.
4. Bind the properties. This is the part that makes it work:

   | On | Bind | To |
   |---|---|---|
   | Attempt Parry, Attack range, Facing, Create Space, Face Target | `Target` | Perception → `Target` |
   | Is Leashed | `HomeLocation` | Perception → `HomeLocation` |
   | Investigate's Move To | destination | Perception → `InvestigateLocation` |
   | Return Home's Move To | destination | Perception → `HomeLocation` |
   | Combat's enter condition | bool | Perception → `bHasTarget` |
   | Investigate's enter condition | bool | Perception → `bHasInvestigateLocation` |

   An unbound `Target` is null on every tick, and every task fails instantly —
   the NPC will stand still and look broken rather than erroring.
5. Set the tree on `DA_NPC_TestDummy` → Behaviour → **State Tree Ref**.

## Step 3 — the level

Drop `BP_NPC_Manny` into `Lvl_ThirdPerson`, then:

- **Add a `NavMeshBoundsVolume`.** The retreat task and both Move To states use
  navigation. Without a navmesh, Create Space logs *"the navmesh offered nowhere
  to go"* and gives up — correctly, but you will think the AI is broken.
- Press **P** in the editor to confirm the navmesh actually generated.

## What to expect, and how to read it

| Behaviour | How to trigger | What proves it |
|---|---|---|
| Sight acquisition | Walk into its 1600cm cone | It turns to face you and closes |
| Retention past the cone | Get behind it mid-fight | It keeps coming — the cone gates acquisition only |
| Hearing | Sprint at it from behind, out of range | It acquires you with no line of sight |
| Investigate | Hit it from outside detection range | It walks to where the blow came from **without** targeting you |
| Parry | Swing at it in range | It raises guard just before contact; jitter means not every time |
| Flee + heal | Drop it below 35% | It backs off and drinks |
| Leash | Run 3000cm from its spawn | It disengages and returns |

The parry is the interesting one. `ReactionJitter` is 0.05s by default, so the
same NPC gives you some perfect parries, some plain blocks and some clean hits —
that spread is deliberate, not flakiness. Turn it to 0 to make it robotic and
verify the timing is right, then turn it back up.

## Known gaps

- **No consumables are stocked by default.** `Recover` will fail its
  `Use Consumable` task until the archetype's inventory has something in slot 0.
  It still backs off, which is the half worth watching first.
- **`ABP_Unarmed` is not a combat anim Blueprint.** The NPC will swing with
  whatever the attack montages drive and otherwise play the unarmed locomotion
  set. Retargeting a proper NPC anim BP is animation work, not AI work.
- **The EQS retreat hook is unwired.** `Create Space` samples the navmesh
  directly; `RetreatQuery` exists on the task for when a query asset gets
  authored. See the note in `ARPGStateTreeTasks.cpp`.
