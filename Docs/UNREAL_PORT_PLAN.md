# ARPG — Godot → Unreal Port Plan

Migration of the combat, magic, and elemental world systems from the Godot GDExtension
project (`C:\Users\young\arpg`) into this Unreal project (`C:\Users\young\unreal\arpg`).

**Target:** Unreal Engine 5.8, runtime module `arpg`.

**Locked decisions**

| Decision | Choice |
|---|---|
| Ability framework | **Gameplay Ability System (GAS)** |
| Scope | **Everything** — combat, magic, elemental reactions, conduction, fire spread, persistent fluids |
| Multiplayer | **Co-op, server-authoritative.** World hazards affect all players, so world state cannot be cosmetic-only |
| Skeleton | **UE5 mannequin**, to keep native/Epic animation content usable |
| Fluid waterline | **Not gameplay-load-bearing.** Wave displacement is cosmetic; gameplay queries use a flat surface |
| Not ported | Terrain, environment art, character meshes, world streaming, weather (all placeholder) |

Source of truth for behaviour is the Godot C++ headers — they carry extensive design
rationale in their class comments. When this plan and a header disagree about *intent*,
the header wins; when they disagree about *mechanism*, this plan wins.

> **Version caveat.** UE 5.8 is newer than my training data. The GAS concepts here
> (GameplayEffectComponents, ability tasks, effect contexts, replication modes) have been
> stable since 5.3 and should hold, but verify exact class names and signatures against the
> 5.8 headers as you go rather than trusting them verbatim.

---

## 0. Before anything else

**Initialise a git repository.** This project is not currently under version control. A port
of this size without history is a bad trade — do this first.

```bash
cd /c/Users/young/unreal/arpg && git init && git add -A && git commit -m "Unreal template baseline"
```

Add a `.gitignore` covering `Binaries/`, `Intermediate/`, `Saved/`, `DerivedDataCache/`,
`.vs/`, and `*.sln`.

**Strip the template.** Delete `Source/arpg/Variant_Combat/`, `Variant_Platforming/`,
`Variant_SideScrolling/` and their `Content/Variant_*` counterparts, then remove the matching
`PublicIncludePaths` entries from `arpg.Build.cs`. They define a competing combat vocabulary
(`CombatCharacter`, `ICombatDamageable`, `AnimNotify_DoAttackTrace`) that will collide
conceptually with the port. Keep `Content/Characters/Mannequins` and `Content/Input`.

---

## 1. Sizing

| Subsystem | Approx. lines | Port character |
|---|---:|---|
| Combat core (damage, status, poise, parry, hitbox) | 2,500 | Mostly absorbed by GAS |
| Combo + attack definitions | 1,200 | Component survives, animation layer rewritten |
| Magic (elements, combination, discharge, cloak) | 3,000 | Component survives, discharge becomes abilities |
| Elemental volumes + reactions + conduction | 1,300 | Near-direct port to subsystems |
| Fire spread | 2,700 | Direct port, new fuel-map + mask backing |
| Persistent fluids (pools, solids, geometry, surface) | 3,500 | Largest rewrite — geometry backend changes |
| Progression / weapon / armor / inventory | 2,500 | Direct port |
| NPC AI (behavior trees, perception) | 2,000 | Retarget onto UE Behavior Trees |
| `animation.gd` | 3,900 (GDScript) | Rewritten; most of it disappears |

---

## 2. Module layout

Five runtime modules in a strict dependency chain, no cycles:

```
ARPGCore     tags, base data assets, attribute sets, effect context, math helpers
   ↑
ARPGCombat   ASC, executions, combat/poise/parry/hitbox, weapons, armor, combo, progression
   ↑
ARPGMagic    elements, combination table, magic component, discharge abilities, ElementalVolume
   ↑
ARPGWorld    reaction / conduction / spread / fluid-surface subsystems
   ↑
arpg         game mode, player state, player controller, character, HUD   (also depends on ARPGAI)

ARPGAI       perception, BT tasks, NPC definitions          (depends on ARPGCore + ARPGCombat)
```

**One deliberate departure from the Godot layout.** In Godot, `ElementalReactionSystem` and
`ConductionSystem` live in `src/magic/`, but they are world solvers that need to reach
`FluidSurfaceSystem` — which would make magic depend on world and close a cycle. Move all four
solvers into `ARPGWorld` and leave `ElementalVolume` (genuinely "a region made of an element")
in `ARPGMagic`. Dependencies then run one direction only.

**Plugins to enable:** `GameplayAbilities`, `GeometryScripting` + `GeometryFramework` (fluid
meshing), `Niagara` (core). Consider `PoseSearch` for motion-matched locomotion (see §6). Add
`GameplayAbilities`, `GameplayTags`, `GameplayTasks` to every Build.cs that touches the ASC.

---

## 3. Networking and authority

Co-op is server-authoritative, and world hazards damage players — so this is a first-class
constraint from phase 1, not a retrofit. It does not all have to be *implemented* early, but
ASC placement, replication modes, and authority checks must be right from the start.

### 3.1 ASC placement

| Actor | ASC lives on | Replication mode | Why |
|---|---|---|---|
| Players | `AARPGPlayerState` | `Mixed` | Attributes must survive pawn death and respawn |
| NPCs | The Pawn | `Minimal` | Dies with the pawn; no owning client needs full GE detail |

Raise `AARPGPlayerState`'s `NetUpdateFrequency` — the default is around 1 Hz and will make
attribute updates visibly lag. The Pawn caches an ASC pointer and implements
`IAbilitySystemInterface`.

### 3.2 Prediction policy

| Abilities | Policy |
|---|---|
| `GA_MeleeAttack`, `GA_Dodge`, `GA_Block`, `GA_DrawSheathe`, `GA_Imbue`, `GA_Consumable` | `LocalPredicted` |
| `GA_Discharge_*` | `LocalPredicted`, with server-authoritative mana drain (below) |
| `GA_Flinch_*`, `GA_StanceBreak`, `GA_Death`, `GA_HitStop` | `ServerInitiated` — these are reactions to server-resolved events |

### 3.3 The charge-drain task under replication

Applying an instant GE every frame does not survive contact with the network. Split it:

- **Client** predicts the charge fraction locally and drives the UI from it.
- **Server** runs the authoritative drain on a fixed cadence (~10 Hz), applying accumulated cost.
- **On release**, the ability sends the reached fraction as target data; the server clamps it to
  what was actually affordable, and that clamped value is authoritative.
- `bForced` (mana ran out mid-charge) is a server decision replicated back to the client.

### 3.4 Hit detection

**Recommendation: server-authoritative traces.** The `AnimNotifyState_Hitbox` runs everywhere,
but only the server's traces produce damage; client traces drive cosmetic VFX timing only.

This is PvE co-op, so the lag-compensation and rewind machinery PvP would demand isn't worth
its cost. The alternative — clients send `FGameplayAbilityTargetData` and the server validates —
is available if hit registration feels bad at higher latency, but don't start there.

### 3.5 RNG must be server-side

`critical_chance` and status `application_chance` currently roll wherever the code happens to
run. Both must roll **on the server only**, with the outcome carried in the effect context
(`bIsCritical` is already a field there). Independent client rolls desync immediately.

### 3.6 World state replication

The Godot spread design already separates *simulation* (per-chunk cell grids) from
*presentation* (a coarser composited mask texture). That split is exactly what makes this
tractable: **replicate the presentation, never the simulation.**

| System | Authority | What replicates |
|---|---|---|
| `SpreadSystem` | Server simulates; all damage server-side | Coarse per-chunk mask as a delta-updated byte array, for client rendering only |
| `FluidSurfaceSystem` | Server owns all pool/solid polygons | `FluidPool` / `FluidSolid` as replicated actors carrying their polygon outline |
| `ElementalReactionSystem` | Server resolves collisions | Nothing extra — the product spawns as a replicated actor |
| `ConductionSystem` | Server floods the graph | Multicast RPC carrying the affected volume list, for VFX |

`FluidSolid` is standable, so its collision must exist on clients. Rebuild mesh **and**
collision client-side from the replicated polygon outline — that keeps server and client
geometry identical without replicating mesh data.

### 3.7 Godot patterns that must change

- `Node::get_instance_id()` (the `int64_t source_id` threaded through `DamageInstance`,
  `ElementalVolume`, and the hitbox) is per-process and **never network-valid**. The plan
  already replaces it with `TWeakObjectPtr<AActor>`; under replication that becomes mandatory
  rather than merely tidier.
- Every `_process()`-driven system that mutates gameplay state needs a `HasAuthority()` gate.
- `FARPGGameplayEffectContext::NetSerialize()` is now load-bearing, not hygiene — every field
  in §4.3 must be serialised or it silently arrives empty on clients.

---

## 4. GAS architecture

Three parts of the Godot design don't map onto GAS by default. Each has a specific answer
here rather than being discovered mid-implementation.

### 4.1 Attribute sets

```cpp
UARPGVitalSet        Health, MaxHealth, Stamina, MaxStamina, Mana, MaxMana, Poise, MaxPoise
                     StaminaRegenRate, StaminaRegenDelay, ManaRegenRate
                     // meta (never replicated, consumed in PostGameplayEffectExecute):
                     IncomingDamage, IncomingHealing, IncomingPoiseDamage

UARPGOffenseSet      AttackPower, CritChance, CritMultiplier, DamageAmpMultiplier

UARPGResistanceSet   PhysicalResistance, FireResistance, ColdResistance, LightningResistance,
                     PoisonResistance, HolyResistance, <one per damage type>
```

`CombatStats` splits across these three. `ResourceComponent` disappears entirely — stamina and
mana become attributes, `spend_*` becomes an ability cost, `restore_*_over_time` becomes a
periodic GE.

Clamp in **both** `PreAttributeChange` (max-value changes, direct sets) and
`PostGameplayEffectExecute` (effect-driven changes). They fire on different paths; skipping
either leaks out-of-range values.

### 4.2 Damage type resistances — the data-driven lookup

*Friction point one: the Godot version keys resistances by string in a dictionary; GAS
attributes are fixed C++ fields.*

```cpp
UCLASS()
class UARPGDamageTypeAsset : public UPrimaryDataAsset
{
    UPROPERTY(EditDefaultsOnly) FGameplayTag          DamageTypeTag;   // Damage.Fire
    UPROPERTY(EditDefaultsOnly) FGameplayTagContainer Categories;      // Damage.Category.Magical
    UPROPERTY(EditDefaultsOnly) FGameplayAttribute    ResistanceAttribute;
    UPROPERTY(EditDefaultsOnly) float                 DefaultPenetration = 0.f;
    UPROPERTY(EditDefaultsOnly) float                 MinResistance = -1.f;
    UPROPERTY(EditDefaultsOnly) FLinearColor          Color;

    UFUNCTION(BlueprintNativeEvent)                  // == GDVIRTUAL _on_damage_applied
    void OnDamageApplied(AActor* Target, const FGameplayEffectSpec& Spec) const;
};
```

`UARPGDamageExecution` declares capture definitions for **all** resistance attributes in its
constructor, holding them in a `TMap<FGameplayTag, FGameplayEffectAttributeCaptureDefinition>`.
At execute time it reads the damage type tag off the effect context and looks up which captured
magnitude to use.

Adding a damage type costs one attribute, one map entry, and one asset — and in exchange
`add_resistance_bonus(type_id, bonus, duration)` becomes a plain duration GE with an Additive
modifier, gaining stacking, expiry, replication, and UI queryability for free. Net win over the
dictionary.

### 4.3 Effect context — the `DamageInstance` replacement

`DamageInstance` carries far more than GAS's default context:

```cpp
USTRUCT()
struct FARPGGameplayEffectContext : public FGameplayEffectContext
{
    TWeakObjectPtr<UARPGHitboxComponent> SourceHitbox;
    FVector  ContactPoint;       bool bHasContactPoint = false;
    FVector  KnockbackDirection; float KnockbackForce  = 0.f;
    float    PoiseDamage      = 0.f;
    float    HitStopDuration  = 0.f;
    float    Penetration      = -1.f;     // -1 = use damage type default
    bool     bUnblockable     = false;
    bool     bIsCritical      = false;    // rolled server-side only — see §3.5
    FGameplayTag MagicElementTag;         // replaces metadata["magic_element_id"]
    TMap<FGameplayTag, float> Metadata;

    virtual UScriptStruct* GetScriptStruct() const override;
    virtual FARPGGameplayEffectContext* Duplicate() const override;
    virtual bool NetSerialize(FArchive&, UPackageMap*, bool&) override;
};
template<> struct TStructOpsTypeTraits<FARPGGameplayEffectContext>
    : public TStructOpsTypeTraitsBase2<FARPGGameplayEffectContext>
{ enum { WithNetSerializer = true, WithCopy = true }; };
```

Register it by overriding `AllocGameplayEffectContext()` on a `UARPGAbilitySystemGlobals`
subclass, and point `DefaultGame.ini` at that class:

```ini
[/Script/GameplayAbilities.AbilitySystemGlobals]
AbilitySystemGlobalsClassName="/Script/ARPGCore.ARPGAbilitySystemGlobals"
```

### 4.4 Status effects

One GameplayEffect asset per status effect. Stacking maps almost cleanly:

| Godot `StackBehavior` | GAS configuration |
|---|---|
| `STACK_REFRESH` | `StackLimitCount=1`, `StackDurationRefreshPolicy=RefreshOnSuccessfulApplication` |
| `STACK_ADDITIVE` | `StackLimitCount=MaxStacks`, refresh on application |
| `STACK_EXTEND` | **No native equivalent** — custom `UGameplayEffectComponent` that reads remaining duration on application and re-applies at `Remaining + DefaultDuration` |
| `STACK_REPLACE` | Remove-then-apply inside the `ApplyStatusEffect` helper |
| `STACK_IGNORE` | Custom application requirement returning false when already present |

- `application_chance` → chance-to-apply GE component (server-rolled — see §3.5).
- `tick_interval` / `tick_damage_per_stack` → GE `Period` plus a periodic execution reading
  `ExecutionParams.GetOwningSpec().GetStackCount()`. Stack-scaled tick damage needs no extra
  machinery.
- `add_status_immunity` → `GrantedApplicationImmunityTags`. Native.

**Keep the authored-asset ergonomics.** In Godot one `.tres` carries icon, VFX scene, fit mode,
VFX priority, and the NPC behavior-tree override alongside combat data. Don't split that into a
second asset — put it on a `UARPGStatusPresentationComponent : UGameplayEffectComponent`
attached to the GE, so **one GE asset is still one status effect**. That component also hosts
the `BlueprintNativeEvent` hooks replacing `GDVIRTUAL _on_apply/_on_tick/_on_remove/
_on_stack_changed`.

### 4.5 Charge-drain mana

*Friction point two: GAS costs are one-shot; discharge drains continuously and forces a release
at whatever fraction was affordable.*

Don't use `CostGameplayEffectClass`. Write one ability task:

```
UARPGAbilityTask_ChargeDischarge
    Inputs : MinCost, MaxCost, UsageRate, MaxChargeTime
    Tick   : Fraction    = Clamp(Elapsed / MaxChargeTime, 0, 1)
             TargetSpend = UsageRate * Lerp(MinCost, MaxCost, Fraction)
             Delta       = TargetSpend - AlreadySpent
             if (Mana < Delta) → spend the remainder, bForced = true,
                                 broadcast OnReleased(AffordableFraction, true), EndTask
             else             → apply instant GE (SetByCaller Data.ManaCost = Delta)
    Output : OnReleased(float ChargeFraction, bool bForced)
```

Run it in parallel with `UAbilityTask_WaitInputRelease`; whichever fires first releases. Override
`CheckCost()` so activation is gated on affording `MinCost`, matching `begin_discharge_charge()`
returning false. See §3.3 for how the drain cadence changes under replication.

`consume_for_imbue` and `consume_for_dodge` stay flat one-shot costs and *can* use a normal cost GE.

### 4.6 Combos

*Friction point three: the six-phase attack with buffering, `continuous_hold`, and channel does
not want to be an ability graph.*

Keep `UARPGComboComponent` as a plain `UActorComponent` holding exactly the state the Godot
version holds — current node, buffered input + window, reset timer, root lockout, per-input held
flags, channel state, attack sequence number. It does **not** play animations. On resolving the
next node it fires:

```cpp
ASC->HandleGameplayEvent(TAG_Event_Attack_Begin, &EventData);
// EventData.OptionalObject = the resolved UARPGAttackDefinition
```

`UGA_MeleeAttack` (triggered by that tag) plays the montage, applies stamina cost, grants tags,
drives phases off anim notifies, and calls `ComboComponent->NotifyAttackFinished()` when the
cancel window opens.

This is where GAS earns its place — three pieces of manual plumbing become declarative:

| Godot | GAS |
|---|---|
| `ComboComponent::set_attack_locked(true)` from flinch | `GA_MeleeAttack.ActivationBlockedTags += State.Flinching` |
| `cancel_attack()` from dodge / hitstun / death | `GA_Dodge.CancelAbilitiesWithTag += Ability.Attack` |
| `PoiseComponent::set_attack_hyperarmor()` fed by `animation.gd` | `GA_Flinch_*.ActivationBlockedTags += State.Hyperarmor`, granted by the attack |

### 4.7 Poise

Poise is an attribute; `IncomingPoiseDamage` is a meta attribute consumed in
`PostGameplayEffectExecute`, which compares the single contribution against
`heavy_flinch_threshold_pct` and `light_flinch_threshold_pct` of `MaxPoise` and sends
`Event.Poise.HeavyFlinch` / `.LightFlinch` / `.StanceBreak`.

Keep a small `UARPGPoiseComponent` for hold-then-drain decay and the break-immunity window.
Expressing "hold for N seconds after the last contribution, then drain at R/s" as GEs means an
infinite regen GE gated by a tag applied and removed by timers — more moving parts than the 40
lines it replaces.

### 4.8 Ability inventory

```
GA_MeleeAttack        GA_AirAttack          GA_Dodge             GA_ElementalDodge
GA_Block              GA_Parry              GA_DrawSheathe       GA_Consumable
GA_Discharge_Burst    GA_Discharge_Emanate  GA_Discharge_Project GA_Discharge_Cloak
GA_Imbue              GA_Flinch_Light       GA_Flinch_Heavy      GA_StanceBreak
GA_HitStop            GA_Death
```

---

## 5. Translation reference

| Godot | Unreal |
|---|---|
| `Resource` (.tres), top-level | `UPrimaryDataAsset` |
| Nested sub-`Resource` (combo tree, BT tree, modifiers) | `UPROPERTY(EditAnywhere, Instanced)` on a `UCLASS(EditInlineNew, DefaultToInstanced)` |
| `GDVIRTUAL` hook | `UFUNCTION(BlueprintNativeEvent)` |
| `Node` child component | `UActorComponent` |
| Signals | `DECLARE_DYNAMIC_MULTICAST_DELEGATE_*` |
| `String` id (`type_id`, element id, tags, factions) | `FGameplayTag` |
| `Dictionary` keyed by id | `TMap<FGameplayTag, T>` |
| `int64_t source_id` (ObjectID) | `TWeakObjectPtr<AActor>` — never network-valid as an int |
| `PackedScene` VFX | `TSoftObjectPtr<UNiagaraSystem>` / `TSubclassOf<AActor>` |
| Group-found singleton (`ElementalReactionSystem::find`) | `UWorldSubsystem` |
| `Area3D` melee hitbox | Swept traces from an `AnimNotifyState` (§6) |
| `Area3D` spell volume | `UBoxComponent` / `USphereComponent` overlap — unchanged |
| `AnimationTree` + manual clip seeking | `UAnimMontage` sections + `UAnimInstance` |
| `blend_locomotion` upper-body overlay | Montage slot + `Layered Blend Per Bone` |
| `get_root_motion_position` plumbing | Montage root motion — delete the code |
| `swing_pitch.gd` spine chain | Control Rig post-process node |
| `impact_twitch.gd` spring bones | `AnimDynamics` node or Physical Animation blend |
| BT `Resource` tree | `UBehaviorTree` + `UBTTask_*` / `UBTDecorator_*` |
| `Blackboard` | `UBlackboardComponent` |
| Godot global shader uniform (`env_spread_mask`) | `UMaterialParameterCollection` + render target |
| `res://` load | `TSoftObjectPtr` + `UAssetManager` |

---

## 6. Animation layer

`animation.gd` is 3,900 lines, and most of it exists to work around Godot's lack of montages,
anim notifies, and built-in root motion. Expect to **delete** far more than you translate.

### Skeleton: UE5 mannequin

Retarget the existing Kimodo/Mixamo output onto the mannequin with IK Rig + IK Retargeter. The
offline generation pipeline (`kimodo_retarget/`, `generate_inbetween.ps1`) needs no changes — only
its output destination does.

**This deletes a whole pipeline.** The Godot project used extended-skeleton GLBs with custom
`root` + `weapon.L` / `weapon.R` bones, generated by `build_extended_models.gd`, because Godot
needed real bones for weapon attachment and root motion. The mannequin already has `hand_r` /
`hand_l` sockets and native root motion, so weapon attachment is socket-based and that entire
extended-skeleton step disappears.

Retarget `swing_pitch` (Control Rig) and `impact_twitch` onto mannequin bone names —
`spine_01..05`, `clavicle_*`, `head`.

Because the skeleton is the mannequin, Epic's animation content is usable directly. Consider the
Game Animation Sample and `PoseSearch` (Motion Matching) for locomotion — it's mannequin-native
and would replace a substantial slice of the locomotion blending work.

### Attack phases become montage sections

One montage per `AttackDefinition`, sections `Windup / Active / Gap / Active2 / Recovery`, plus
`Landing` for air attacks. The six `*_animation_name` string fields collapse into one montage
reference.

- `chargeable` — set the Windup section's play rate to `ClipLength / ChargeTime`; release jumps to
  `Active` at the fraction reached. (Godot time-stretched the clip manually; same idea, natively.)
- `channel` — loop the `Active` section until input release or stamina exhaustion.
- `blend_locomotion` — montage plays on the `UpperBody` slot instead of `FullBody`; the AnimBP
  does `Layered Blend Per Bone` from `spine_01`.
- `movement_speed_factor` — applied to `MaxWalkSpeed` by the ability, restored on end.

### Notifies replace the manual phase driver

| Notify | Replaces |
|---|---|
| `UAnimNotifyState_Hitbox` (carries `HitboxSource`, `TickInterval`) | `_set_hitbox_active()` |
| `UAnimNotify_ComboWindowOpen` | `notify_attack_finished()` |
| `UAnimNotify_ChargeReleasePoint` | `_discharge_trigger_time_for()` |
| `UAnimNotify_FootstepNoise` | `NoiseComponent` emission |

### Melee detection

Keep the `UARPGHitboxComponent` contract intact — base damage, poise damage, on-hit effects,
`bOneShot`, `TickInterval`, faction filter, per-activation hit set, deferred self-overlap
suppression. Replace overlap events with **swept multi-sphere traces between the previous and
current socket transforms**, server-authoritative per §3.4.

The Godot version already publishes `hitbox_world_position` every frame precisely because overlap
detection tunnels on fast swings. Traces solve that properly and also yield an exact contact point
for `impact_twitch` and wound placement without the closest-approach solve.

Spell volumes (`ElementalVolume`) stay overlap-based — they're slow and area-shaped, which is what
overlap is good at.

---

## 7. World systems

All four solvers become `UWorldSubsystem`s, server-authoritative per §3.6. The
`MagicCombinationTable` stays a single shared asset with the `Scope` bitmask
(`HAND / COLLISION / FIELD / SURFACE`) and `ReactionMode` (`AUTO / CONDUCT / SOLIDIFY`) intact —
that design is why one table serves four consumers, and it survives the port unchanged.

### Elemental reactions + conduction

Near-direct ports. `ElementalVolume` keeps its energy / reservoir / absorption / conductivity
model. The overlap-graph flood in `ConductionSystem` maps straight onto
`GetOverlappingComponents()`. The duck-typed `on_elemental_reaction(consumed, remaining, product)`
hook becomes a `BlueprintNativeEvent` on an `IElementalReactive` interface, keeping the same
default fallback (scale hitbox, cube-root scale the owner, finish when spent).

### Fire spread

Direct port of the two-layer design (object layer over a spatial hash, per-chunk cellular field
layer, active-set ticking). Two backing pieces change:

- **Fuel map.** `SpreadFuelMap` is baked from Terrain3D grass density. Replace with UE Landscape
  layer weights, baked offline into a per-cell `UTexture2D` — mirroring the existing `world_baker`
  addon rather than sampling the landscape at runtime.
- **Visual mask.** The `env_spread_mask` global shader uniform becomes a
  `UMaterialParameterCollection` plus a `UTextureRenderTarget2D` written from the subsystem. MPC
  parameters are the direct analogue of Godot's `[shader_globals]`.

Damage delivery stays as designed — walk registered dynamic targets and sample the field under
them, routing through the hurtbox so i-frames and dodging still apply. Do **not** spawn per-cell
collision.

### Persistent fluids

The largest rewrite. The design — every body is a polygon, so depositing is a union, rain and
evaporation are outward/inward offsets, and freezing is an intersection — is sound and worth
preserving exactly. What changes is the backend:

- `Geometry2D` (Godot built-in) has no UE equivalent. Use **Clipper2**, already vendored in the
  engine under `GeometryProcessing`, for union / offset / intersect.
- `fluid_geometry.cpp`'s procedural meshing → `UDynamicMeshComponent` (GeometryFramework) or
  `GeometryScript` polygon extrusion.
- **The waterline is cosmetic.** `ElementalVolume::contains_point()` becomes polygon XZ
  containment against a flat surface height — no query back into the wave simulation. Wave
  displacement lives entirely in the material/vertex shader with no gameplay coupling, which also
  removes it as a replication concern. The `M_ocean` shader is still a rewrite, but a
  presentation-only one.

Keep the broadphase-vs-truth split from the Godot version regardless: the tall collision box is
deliberately *not* the fluid body (the shipped river is 7 m deep and cuts into the banks).

---

## 8. Content migration

108 `.tres` files exist; roughly 60 are in scope (the rest are terrain, weather, and chunk
manifests being dropped).

| Content | Count | Approach |
|---|---:|---|
| Sword moveset (`AttackDefinition`) | 17 | **Write a converter.** ~40 fields each; `.tres` is INI-ish and trivially parsed. Use UE Editor Python to emit data assets |
| Consumables | 12 | Converter |
| Damage types | 7 | Converter or hand-author |
| Progression curves | 7 | Hand-author |
| Magic elements + combinations | ~10 | Hand-author |
| Behavior trees | 5 | Hand-author as UE BT assets |
| Status effects | 4 | Hand-author as GE assets — the GAS mapping isn't mechanical |

Everything except the moveset and consumables is small enough that hand-authoring beats writing a
converter — and you'll want to retune during the port anyway.

---

## 9. Phase plan

Each phase ends at something verifiable in-editor. From phase 1 onward, verify in **PIE with 2
clients** (`Net Mode: Play As Listen Server`, 2 players) — catching authority bugs at the phase
that introduces them is far cheaper than auditing later.

| # | Phase | Gate |
|---|---|---|
| 0 | git init, strip template, create 5 modules, enable GAS, tag registry, custom `AssetManager` + `AbilitySystemGlobals`, ASC on PlayerState | Project compiles; `InitGlobalData()` runs; 2-client PIE connects |
| 1 | Attribute sets, `FARPGGameplayEffectContext` (+`NetSerialize`), `UARPGDamageExecution`, hitbox/hurtbox, faction filter | Swing at a dummy — correct number out, resistances applied, health replicates to both clients |
| 2 | Status effects as GEs, presentation component, all five stack behaviors, status VFX manager | Burning ticks, stacks, expires, shows VFX on both clients, is resisted correctly |
| 3 | Poise (attribute + component), flinch/stance-break abilities, parry, block, hit-stop | Light/heavy flinch and stance break all trigger and play remotely |
| 4 | Mannequin retarget, montages, notifies, weapons, armor, `AttackDefinition` assets, `ComboComponent`, `GA_MeleeAttack` | Full sword moveset plays, branches, buffers, charges, channels — predicted client-side |
| 5 | Magic: elements, loadout pages, combination table, complexity gating, all discharge types, imbue, elemental dodge, cloak | fire+water→steam; cast, imbue, dodge all work; charge drain is server-clamped |
| 6 | `ElementalVolume`, reaction subsystem, conduction subsystem | Fireball into water jet produces steam at the right contact point; lightning floods a puddle chain and hurts a *second player* standing in it |
| 7 | Progression trackers, inventory, quick slots, consumables | Mastery multiplies discharge damage; quick-slot potions work |
| 8 | NPC AI — perception, behavior trees, reactive parry | NPC engages, parries a telegraphed swing, flees and heals |
| 9 | Fire spread subsystem (+ landscape fuel bake, MPC mask, replicated mask) | Grass fire crosses a clearing, burns a second player, rain quenches it |
| 10 | Fluid surface subsystem (+ Clipper2 backend, dynamic meshing, replicated outlines) | Water pools; ice shard freezes a floe both players can stand on |

Phases 1–3 are close to mechanical transcription. **Phase 4 is the largest single risk** — it
combines the animation rewrite with the mannequin retarget. Phases 9–10 are self-contained and
can slip without blocking anything else.

---

## 10. Known traps

1. **`UAbilitySystemGlobals::Get().InitGlobalData()`** must be called from a custom
   `UAssetManager::StartInitialLoading()`. Omitting it breaks ability prediction and crashes on
   target data. The single most common GAS setup failure.
2. **Custom effect context** needs all of `GetScriptStruct()`, `Duplicate()`, `NetSerialize()`,
   plus `TStructOpsTypeTraits` with `WithNetSerializer` and `WithCopy`. Missing `Duplicate()`
   silently drops your fields on effect application.
3. **`AbilitySystemGlobalsClassName`** must be set in `DefaultGame.ini` or your globals subclass
   is never used.
4. **`PlayerState` `NetUpdateFrequency`** defaults far too low for an ASC. Raise it or attribute
   changes visibly lag.
5. **Meta attributes** must be consumed *and reset to zero* in `PostGameplayEffectExecute`.
   Reading them anywhere else gives stale values.
6. **Instanced UObject trees** (`ComboAttackNode`, BT nodes) need `UCLASS(EditInlineNew,
   DefaultToInstanced)` **and** `UPROPERTY(Instanced)`. Miss either and the editor gives you an
   unassignable null.
7. **Decide early whether GEs are C++ classes or Blueprint assets.** Recommendation: C++ base
   classes carrying structure, Blueprint assets for per-effect tuning.
8. **`SpreadSystem` reads affliction from status effects, never from its own state.** Keep that —
   it's what lets a fireball's on-hit effects and a wet status steer spread for free. In GAS terms,
   query owned tags on the ASC.
9. **Client-side RNG desyncs.** See §3.5 — crit and status-application rolls are server-only.
