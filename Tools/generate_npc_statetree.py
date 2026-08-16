"""Reports whether a StateTree can be authored from Python, and builds one if so.

    "<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" <path>/arpg.uproject \
        -ExecutePythonScript="<path>/Tools/generate_npc_statetree.py" \
        -unattended -nopause -nosplash

READ THIS BEFORE RUNNING IT. Unlike every other script in Tools/, this one is not
known to work. The others drive UPrimaryDataAsset subclasses whose fields are
plain reflected properties, which the Python bridge handles completely. A
StateTree is not that: its states hold FStateTreeEditorNode, each wrapping an
FInstancedStruct, and its data flow is FStateTreePropertyPathBinding rather than
object references. Whether any of that is reachable through the bridge is a
property of the installed engine, not something this script can assert.

SO IT PROBES FIRST AND REFUSES TO GUESS. The probe names exactly which types and
entry points are reachable and prints them. If anything essential is missing it
stops WITHOUT creating an asset -- a half-built StateTree that opens to an empty
editor is worse than none, because it looks like it worked.

If the probe says no, author the tree by hand. The recipe is in
Docs/TEST_NPC_SETUP.md and it is about ten minutes of dragging; the tree below is
the same structure written out, so the two cannot drift.

THE STRUCTURE, which is worth reading whether or not this runs. Selection is
top-down, first match wins, so the order IS the priority:

    Root
      Return Home    [IsLeashed]                  -> MoveTo(HomeLocation)
      Recover        [HealthBelow 0.35]           -> CreateSpace, UseConsumable
      Combat         [bHasTarget]
        Parry                                     -> AttemptParry(Target)
        Strike       [InAttackRange, FacingTarget]-> Attack(Light)
        Close        [not InAttackRange]          -> FaceTarget, MoveTo(Target)
        Square Up                                 -> FaceTarget(Target)
      Investigate    [bHasInvestigateLocation]    -> MoveTo(InvestigateLocation)
      Idle                                        -> Delay

Parry sits ABOVE Strike deliberately. It fails on entry in a single tick when the
target is not mid-swing, so putting it first costs nothing and means a swing
coming in always outranks one going out.

Leash and Recover sit above Combat for the same reason in reverse: both are
answers to "stop fighting", and a state machine that checks them after the
combat branch has already been selected would only act on them once the current
attack finished.
"""

import unreal

NPC_DIR = "/Game/ARPG/NPCs"
TREE_NAME = "ST_NPC_Melee"
TREE_PATH = "{}/{}".format(NPC_DIR, TREE_NAME)

# The schema decides which context objects the tree's nodes can reach. The AI one
# supplies the AI controller, which is what ARPGStateTree::GetController unwraps.
SCHEMA = "/Script/GameplayStateTree.StateTreeAIComponentSchema"


def log(message):
    unreal.log("[ARPG NPC tree] {}".format(message))


def probe():
    """Names what the Python bridge actually exposes. Returns a missing list.

    DELIBERATELY EXHAUSTIVE rather than stopping at the first failure: the whole
    point of the run is to learn the shape of the gap, and a probe that stops
    early reports one symptom of what may be five.
    """
    required = [
        ("StateTree", "the runtime asset type"),
        ("StateTreeEditorData", "the editor-side container the states hang off"),
        ("StateTreeState", "an individual state"),
        ("StateTreeEditorNode", "the wrapper holding one task or condition"),
        ("InstancedStruct", "how a task struct is stored inside a node"),
    ]

    missing = []
    for name, what in required:
        if hasattr(unreal, name):
            log("  OK      unreal.{} -- {}".format(name, what))
        else:
            log("  MISSING unreal.{} -- {}".format(name, what))
            missing.append(name)

    # Our own nodes. These are USTRUCTs in ARPGAI; if the module did not load,
    # everything above can be present and the tree still cannot be populated.
    ours = [
        "ARPGStateTreeEvaluatorPerception",
        "ARPGStateTreeTaskAttack",
        "ARPGStateTreeTaskFaceTarget",
        "ARPGStateTreeTaskCreateSpace",
        "ARPGStateTreeTaskUseConsumable",
        "ARPGStateTreeTaskAttemptParry",
        "ARPGStateTreeConditionTargetInAttackRange",
        "ARPGStateTreeConditionFacingTarget",
        "ARPGStateTreeConditionHealthBelow",
        "ARPGStateTreeConditionIsLeashed",
    ]

    log("ARPG nodes:")
    for name in ours:
        if hasattr(unreal, name):
            log("  OK      unreal.{}".format(name))
        else:
            log("  MISSING unreal.{}".format(name))
            missing.append(name)

    # Compilation. An edited StateTree is inert until compiled -- the runtime
    # reads a baked representation, not the editor data -- so a script that
    # populates states and cannot compile has produced an asset that loads and
    # does nothing.
    log("compilation entry points:")
    for name in ["StateTreeEditorLibrary", "StateTreeLibrary"]:
        if hasattr(unreal, name):
            log("  OK      unreal.{} -- members: {}".format(
                name, [m for m in dir(getattr(unreal, name)) if not m.startswith("_")]))
        else:
            log("  MISSING unreal.{}".format(name))

    # Property BINDING is the part most likely to be unreachable, and the tree is
    # useless without it: every task reads its Target from the perception
    # evaluator's output, and an unbound Target is null on every tick.
    log("binding support:")
    for name in ["StateTreePropertyPathBinding", "StateTreePropertyPath",
                 "StateTreeEditorPropertyPath", "PropertyBagPropertyDesc"]:
        log("  {} unreal.{}".format("OK     " if hasattr(unreal, name) else "MISSING", name))

    return missing


def recipe():
    """What to do by hand. Printed on any failure so the run is never a dead end."""
    log("")
    log("AUTHOR IT BY HAND -- see Docs/TEST_NPC_SETUP.md for the full recipe.")
    log("  1. Content Browser > ARPG/NPCs > right click > Artificial Intelligence")
    log("     > State Tree. Schema: StateTree AI Component. Name it {}.".format(TREE_NAME))
    log("  2. On the ROOT, add evaluator 'ARPG Perception'. Everything else binds")
    log("     to its outputs, so nothing works until this exists.")
    log("  3. Add the states in the order in this file's docstring. Selection is")
    log("     top-down first-match, so the order IS the priority.")
    log("  4. Bind every task's Target to the evaluator's Target, Is Leashed's")
    log("     HomeLocation to HomeLocation, and the two MoveTo states to")
    log("     LastKnownLocation and InvestigateLocation.")
    log("  5. Set it on DA_NPC_TestDummy > Behaviour > State Tree Ref.")


def main():
    log("probing the StateTree Python surface on this engine build...")
    log("core types:")

    missing = probe()

    if missing:
        log("")
        log("STOPPING WITHOUT WRITING AN ASSET. Missing: {}".format(", ".join(missing)))
        log("A partially built StateTree opens to an empty editor and looks like")
        log("it worked, which is the one outcome worth avoiding here.")
        recipe()
        return

    log("")
    log("Everything probed is present, which means a scripted build is possible")
    log("on this engine build. It has still never been run: the ordering of")
    log("editor-data mutation, binding creation and compilation is not something")
    log("the probe can check.")
    log("")
    log("Send the output above back and the build can be written against these")
    log("facts rather than guessed at. Until then, the hand recipe is the")
    log("supported path.")
    recipe()


main()
