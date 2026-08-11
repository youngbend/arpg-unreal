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
| `STACK_EXTEND` | `StackDurationRefreshPolicy=ExtendDuration` — **native**, adds the new spec's duration onto the remaining time |
| `STACK_REPLACE` | `StackLimitCount=1` + refresh. Not identical: GAS refreshes in place rather than removing and re-adding, so any on-remove side effect does not fire. Only matters for effects that have one |
| `STACK_IGNORE` | `ApplicationTagRequirements.IgnoreTags` containing the effect's own granted status tag — **native**, a second application is blocked while the first is active |

> **Corrected 2026-08-07.** This table previously claimed `STACK_EXTEND` had no
> native equivalent and needed a custom component, and that `STACK_IGNORE` needed
> a custom application requirement. Both are native in 5.8. Verified against
> `EGameplayEffectStackingDurationPolicy` in `GameplayEffect.h` and the
> `CarryOverDuration` path in `FActiveGameplayEffectsContainer`. All five Godot
> stack behaviours are therefore configuration, not code.

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

### Status

**Phase 0 — done.** Five modules, GAS wired, tag registry. Verified by a headless
boot logging `AbilitySystemGlobals init complete (globals class:
ARPGAbilitySystemGlobals)`, which is the proof the ini registrations actually
resolved rather than silently falling back to the base classes.

**Phase 1 — done.** Attribute sets, damage execution, hitbox/hurtbox, faction
filter, seven damage type assets. Verified three ways:

- `ARPG.Combat.DamageMitigation` (11 cases), `ARPG.Combat.FactionHostility`
  (13 assertions), `ARPG.Combat.HitboxDetection` (9 assertions incl. tunnelling).
- MCP inspection of a live PIE session: attribute sets registered, faction tags
  applied, `BeginPlay` initialisation confirmed on both the dummy and the
  PlayerState-owned player ASC.
- 2-client listen-server PIE: one swing against two dummies with different
  mitigation, both replicating to the client:

  | Dummy | Armor / Resist | Damage |
  |---|---|---|
  | control | 0 / 0 | 50 |
  | mitigated | 10 / 0.5 | 20 |

  20 is the number that distinguishes the orderings: resistance-first would give
  15, armor-only 40. This is the live confirmation that armor applies flat,
  physical-gated, before resistance.

**Phase 2 — done.** Status effects as GEs, stack behaviours, application
gating, resistance. `ARPG.Combat.StatusEffects` covers all five stack modes and
the resist path. StatusVfxManager deferred — it is presentation, and nothing
downstream reads it. **Landed in phase 12.**

**Phase 3 — done.** Poise attribute, two-tier flinch, stance break, parry and
block, interception folded into the execution. `ARPG.Combat.Poise` covers the
tiers and the interception ordering. Hit-stop deferred with the VFX manager;
**both landed in phase 12.**

**Phase 4 — code complete.** Retarget, combo machine, melee ability, weapons,
armour, charge and channel are all in. What remains is content: 14 of the 15
montages still need their sections and notifies placed by hand.

- 42 paladin clips retargeted onto the UE5 mannequin via IK Rig / IK Retargeter.
- `ARPG.Combat.Combo` covers tree traversal, buffering, timeout and stamina.
- `ARPG.Combat.Equipment` (5 cases) covers equip, swap and unequip for both
  weapons and armour.
- Confirmed in game: the sword moveset fires from `ARPGAttack` against a live
  montage with hitbox notifies.

Charge and channel are driven as MONTAGE SECTIONS rather than as separately
sequenced clips. A charge time-stretches `Windup` to take `ChargeTime` -- the
rate derived from the section's own length, so re-timing a clip cannot desync the
charge -- and jumps to `Active` on release. A channel self-links `Active` so it
loops, and relinks it to `Recovery` to break out. Neither needs a notify to
announce a phase boundary, because a montage knows its own section times; this is
the concrete payoff of the animation rewrite, and it is what let ~a third of
Godot's 3,900-line `animation.gd` disappear.

Three ordering decisions worth recording, all verified by
`ARPG.Combat.Charge.*` / `ARPG.Combat.Channel.*` (8 cases):

- **A charge starts its ability at the PRESS, not the release.** The wind-up has
  to be on screen while the player holds -- it is both their charge feedback and
  the opponent's tell. Stamina is therefore deferred to the release, and an
  interrupted charge costs nothing.
- **A channel pays at the press but drains only during the loop.** The wind-up is
  the attack starting, not something to back out of; but charging for the wind-up
  and recovery would make a long wind-up a tax.
- **`CancelAttack` must clear the channel flags.** The ability that would
  otherwise call `NotifyChannelEnded` is the thing being cancelled, so nothing
  else would ever clear them and every later press would be swallowed.

Two decisions made during the equipment port that depart from the Godot source:

1. **Equipment writes attributes on equip rather than being consulted at hit
   time.** Godot's `receive_damage()` reached into `ArmorComponent` mid-pipeline.
   Here armour adds to `BaseArmor` and the resistance attributes on equip and
   removes exactly that on unequip, so the execution reads one number per stat
   and everything that can move that number composes through one aggregator.
   The removal path subtracts *what was applied*, not what the definition
   currently says — otherwise a reroll or a live asset edit leaves permanent
   drift. `ArmorEditedWhileWornStillReverses` pins this.
2. **Weapon crit is wired up.** Godot had `get_total_crit_chance_bonus()` on both
   `WeaponDefinition` and `WeaponComponent`, but nothing ever called either — a
   weapon's crit affix was dead data. In the port the bonus goes onto the
   wielder's `CritChance` attribute, and the hitbox rolls against that attribute
   *plus* its own per-attack `CriticalChance` rather than the hitbox value alone.
   Adding rather than replacing keeps an authored "this finisher crits more"
   meaningful across every weapon.

Armour resistances are keyed by the damage type **asset**, not its tag, because
the asset already names which resistance attribute answers for it; keying by tag
would need a parallel tag→attribute lookup that could drift out of step. An asset
with no `ResistanceAttribute` set logs a warning rather than silently doing
nothing — `ArmorWarnsOnUnmappedResistance` pins that too.

**Phase 5 — code complete.** Elements, palettes, loadout pages, the combination
table, complexity gating, all four discharge types, imbue, elemental dodge and
cloak. 19 new cases under `ARPG.Magic.*`; 38 pass in total.

The gate the plan set — fire+water→steam — is `ARPG.Magic.Combination.
FireAndWaterMakeSteam`. Casting itself is not covered by tests: the discharge
abilities spawn effect actors and play montages that need authored content, so
the formulas they consume are tested through `BuildDischargeContext` instead.
`AARPGMagicCaster` is the placeable equivalent of the combat dummy for trying it
in-editor, and supplies its own mastery levels.

Structural decisions:

- **The magic component decides; the abilities cast.** Same split as the combo
  component and the melee ability, for the same reason: selection state outlives
  any one cast and has to survive being interrupted, which GAS models badly,
  while a cast is predicted, interruptible and animation-driven, which GAS models
  well.
- **Charge drain is an ability task, not a cost GE** (§4.5 as planned). The
  deciding detail is that running dry does not FAIL a discharge — it fires at
  whatever fraction was paid for, and a cost effect can only answer yes or no.
  Spending is cumulative rather than per-frame, so the price of a given charge
  does not depend on frame rate, and a forced release can solve exactly which
  fraction the spent mana bought.
- **Progression is an interface, not a dependency.** Complexity gating and
  mastery scaling are phase 5 (how casting works); the tracker that computes them
  is phase 7 (how it is earned). `IARPGMagicProgression` narrows the coupling to
  two questions, so phase 5 is complete and testable now. A caster that does not
  implement it is not ungated-by-accident — that is the NPC case, and it is
  pinned by `Gating.NoProgressionMeansNoGate`.
- **Per-type knobs are a map keyed by the discharge type**, not sixteen parallel
  floats behind four switches. Adding a discharge type stops meaning "find every
  switch".
- **Consumption rates are keyed by element tag**, not a second array parallel to
  the reactants. The Godot pairing was by index, which silently mis-assigns every
  rate the moment a reactant is inserted rather than appended.

Two things were found dead during this phase and fixed:

1. **`OnHitEffects` was authored on attacks since phase 4 and read by nothing.**
   The hitbox had nowhere to put them, so no attack could ever inflict a status.
   The hitbox now carries `OnHitEffects` plus a duration override, and the melee
   ability passes its authored list through. Magic depends on the same path —
   every element's status lands this way.
2. **`State.Invulnerable` was defined in phase 0 and honoured by nothing**, so
   dodge i-frames would have been inert. `UARPGHurtboxComponent::IsInvincible`
   now checks it, which also means any effect granting it works.

**Phase 6 — code complete.** Elemental volumes, the reaction subsystem and the
conduction subsystem. 12 new cases under `ARPG.World.*`; 50 pass in total.

The first half of the plan's gate -- fire into water produces steam at the right
contact point -- is `Reaction.MatchedProjectilesBothSpend` plus
`Reaction.ReservoirIsNotDepleted`, which asserts the contact lands on the
waterline rather than the collider centre. The second half (a puddle chain
hurting a second player) is covered as far as it can be without fluids:
`Conduction.ChargeFollowsTouchingMedia` proves the chain, and damage goes through
the ordinary hurtbox and damage execution, but the puddles themselves are phase
10.

Where the module split paid off:

- **Volumes live in `ARPGMagic`; the solvers live in `ARPGWorld`**, as planned,
  because phase 10 needs the solvers to reach the fluid system. That means a
  volume cannot call the reaction subsystem directly without closing a cycle, so
  a volume ANNOUNCES that it met something (a static `OnVolumesMet` delegate) and
  the subsystem subscribes. Subscribers filter by world -- the delegate is
  process-wide, and a PIE server and client would otherwise resolve each other's
  collisions.
- **`ElementalVolume` is a component over an assigned collider**, not a
  shape-derived class. A spell is a sphere and a river is a long box; one
  component serving both beats two classes sharing all their behaviour.

Three corrections to the Godot behaviour, all found by porting it:

1. **Default reaction scaling compounded.** The Godot version multiplied the
   volume's CURRENT scale by the remaining fraction on every reaction, so two
   reactions leaving 0.5 then 0.25 of the original energy landed at 0.5x linear
   instead of 0.63x -- a twice-reacted spell shrank faster than its energy fell.
   Scaling is now absolute against a captured base scale.
2. **Geometry must come from the COLLIDER, not the component.** They coincide
   only when the component sits at the shape's origin, and nothing enforces
   that. Contact points, conduction hop distances and immersion all read
   `GetVolumeLocation()` now; the alternative is reactions resolving at a
   position nothing is actually at.
3. **The conduction graph queries live rather than reading the overlap cache.**
   That cache is maintained as a side effect of MOVEMENT, so it is empty for
   anything standing still -- every reservoir and every puddle. A chain of
   stationary puddles conducted to nothing. Conduction is rare enough that a real
   overlap query costs nothing worth saving. Begin-overlap still needs a movable
   collider, and a static one now warns.

Also wired: conduction delivers damage through the SAME execution every other hit
uses, so resistance, armour and poise behave identically to being hit by the
spell itself, and it is routed through the hurtbox so i-frames and dodging still
apply.

**Phase 7 — code complete.** Progression trackers, the visible level, inventory,
quick slots and consumables. 13 new cases; 63 pass in total.

The plan's gate is met on both halves: `Progression.Tracker.
MasteryMultipliesDischargeDamage` drives a real tracker through the magic
component and asserts the damage, and `Inventory.QuickSlots.
UsingSpendsOneAndStartsTheCooldown` covers the potion path end to end.

**`IARPGMagicProgression` is now implemented.** Phase 5 deliberately left it
open; `UARPGMagicProgressionTracker` closes it. The magic component now looks for
the interface on the owner's COMPONENTS before the actor, so the real tracker
wins over the test caster's stub. Both branches of the gate are pinned: a caster
with no progression is exempt, and a caster with an empty tracker is gated.

Structural decisions:

- **Three Godot trackers became one base plus three thin subclasses.** They
  differed only in what they listened to and what they called a subcomponent;
  the level model, mastery window and multiplier were duplicated across all
  three. Keying every category by gameplay tag -- `Element.Fire`,
  `Weapon.Sword`, `Armor.Heavy` are all just "which thing did you use" --
  collapses the duplication.
- **Mastery is a window query, not a fourth stored level.** It is the fraction of
  recent use attributed to one subcomponent, so it FADES when the player switches
  -- which a stored number could not express without a decay tick. Its floor at
  proficiency 3 is what stops a decaying window silently re-locking combinations
  the player had unlocked.
- **Allocation recomputes an absolute total**, never a delta. Re-applying a save
  has to land on the same number; a delta compounds every point on every load,
  which is invisible in one session and unbounded across many. Pinned by
  `Level.AllocationIsIdempotent`.
- **XP sources differ deliberately.** Weapons progress on LANDING hits, magic on
  CASTING at all. Skill with a sword is about connecting; skill with an element
  is about use, and requiring a hit would leave a purely healing or debuffing
  element unable to progress. Armour progresses on being hit, which is the only
  thing armour does.

**Consumables became GameplayEffects.** Godot's `ConsumableEffect` carried a
ten-value enum -- restore, restore-over-time, damage amp, resistance buff, status
immunity -- with a switch applying each. Every one of those is exactly what a GE
is, and keeping the enum would mean a second, worse effect system inside a
project that already has GAS. The cost is that "restore 50 health" is an asset
rather than a number; the gain is stacking, refresh, dispel and resistance for
free, plus effects the enum could never express.

Two small extensions this phase required:

- `FARPGOnHitLanded` now carries the damage it applied, pre-mitigation. A hit
  that landed without saying how hard is not much use to a listener, and
  pre-mitigation is the right basis for progression: earning less proficiency for
  fighting an armoured enemy would punish the player for the fight being harder.
- `UARPGMagicComponent` raises `OnDischargeExecuted`, the port of Godot's
  `discharge_executed` signal and the magic tracker's XP source.

**Phase 8 — code complete.** Perception, behaviour-tree nodes and the reactive
parry. 8 new cases; 71 pass in total.

**The tree MACHINERY was deleted, not ported.** Godot hand-rolled BTNode,
BTComposite, BTDecorator and their per-node blackboard state because Godot has no
behaviour trees; UE has all of it, with an editor and a gameplay debugger. What
ported is only the domain: five decorators and tasks that read this game's combat
components, plus the parry. UE's node memory replaces the blackboard-keyed
per-node state trick exactly -- one tree asset shared by every goblin, with each
instance keeping its own timers.

**The reactive parry is where phase 4 pays off.** Its fallback timing reads the
target's ACTIVE MONTAGE -- time left in Windup, at the montage's own play rate.
Godot could not do that: AttackDefinition stored clip NAMES with no access to
durations, so `animation.gd` published `time_until_hit_estimate` every frame
purely so the AI could read it. A montage knows its own section times, and that
whole channel disappears into one query.

Its primary path is unchanged and better than a timing constant: it tracks the
target's live weapon hitbox, solves kinematically for time to contact, and
compares against a threshold DERIVED from this actor's own parry component
(`BlendInTime + ParryWindow/2`) so the perfect window is centred on predicted
contact. Guard timings are never duplicated as a second set of knobs.

**Perception is NOT UAIPerceptionComponent**, deliberately. The design's rule is
that the sight cone gates ACQUISITION but not retention -- an NPC mid-fight does
not lose its target by turning its back. UE's sight sense drops a target the
moment it leaves the cone, so expressing this on top of it means reimplementing
retention anyway and then fighting the sense's own forget timer. The three
channels are ~150 lines, deterministic, and testable without a perception system
tick; `Perception.ConeGatesAcquisitionButNotRetention` pins the rule directly.

Hearing is its own channel and ignores range, cone and line of sight entirely: a
loud enough sound is heard through a wall and from behind. Noise is a POLLED
RADIUS rather than UE's event-based hearing sense, because it is a continuous
property of how a character is moving -- sprinting is loud for as long as you
sprint, which an event stream would have to synthesise a tick rate to represent.

**Phase 9 — code complete.** The fire spread field, the fuel map, cross-medium
attrition and contact damage. 7 new cases; 78 pass in total.

Both halves of the plan's gate are covered: `Spread.ReachScalesWithEnergyPutIn`
shows a fire crossing open ground, and `Spread.RainQuenchesFire` puts it out.
Burning a second player is the contact-damage path, which routes through the
hurtbox and so is the same delivery every other damage source uses.

**THE PROPERTY WORTH PROTECTING is that reach is bounded by energy.** Under the
conserved budget, every unit of exposure a burning cell delivers is paid for out
of a finite bank -- seeded by whatever lit it, topped up by burning fuel. That
makes "how far does a fire travel" a linear function of "how much went in"
instead of a binary stall-or-firestorm. It is also one tuning slip from being
wrong, because the sub-critical condition (a cell yields less over its life than
lighting the next one costs) is a relationship between three authored numbers
that no code enforces -- so the definition's validation now states it as a
warning, and two tests pin it from either side.

Two bugs the tests found, both in code written this phase:

1. **Media were rebuilt only on seeding.** Setting fuel before the first seed
   silently did nothing -- the medium it named did not exist yet -- so a
   firebreak painted that way simply burned. Every entry point now ensures the
   media list, including the const queries, which is why the cache is mutable.
2. **A cell driven cold by ATTRITION kept its energy bank.** The cooling branch
   that drops the bank was only reached by a cell that cooled naturally; one
   quenched by rain skipped it entirely, so the moment the rain stopped the fire
   could re-ignite at the reach it had before.

Structural notes:

- **Fuel is baked, not sampled.** Landscape foliage density is expensive to query
  per cell and does not change during play, so baking makes "bare dirt cannot
  carry a fire" a property of the WORLD rather than something the tick
  rediscovers every step -- and a firebreak becomes something a designer paints.
  A chunk with NO entry means "never baked" and falls back to full fuel, so an
  unbaked test level burns rather than being silently fireproof.
- **Attrition comes off the shared combination table's Field-scope rows.** Rain
  quenching a grass fire and a water jet punching through a fireball read
  identical numbers -- one authored row, two solvers.
- **Chunks are allocated on demand and freed when inert**, so memory tracks what
  is alight rather than everywhere a fire has ever been. The active set means a
  map-wide firestorm and a single burning bush each cost in proportion to what is
  actually burning.

**Phase 10 — code complete. All ten phases are now ported.** Persistent fluids:
polygon bodies, deposit merging, rain and evaporation, freezing and melting. 8
new cases; 86 pass in total.

The gate -- an ice shard freezing part of a pool into something a player can
stand on -- is `Fluid.Solidify.FreezesTheOverlapIntoAStandableSlab`.

**The polygon backend is NOT raw Clipper2, and this is a correction to the
plan.** Clipper2 IS vendored under GeometryProcessing, but in
`GeometryAlgorithms/Private`, so it cannot be included from another module. Its
PUBLIC wrapper turned out to be a better fit than the plan anticipated:
`PolygonsUnion` / `PolygonsOffset` / `PolygonsIntersection` /
`PolygonsDifference` already speak in `FGeneralPolygon2d`, which is exactly the
polygon-with-holes type the design wants -- so the four operations the design
reduces to map one-for-one onto public engine API, with no vendoring and no
module surgery.

The design survived the port unchanged, which is the point worth recording:
depositing is a union, rain and evaporation are offsets of opposite sign, and
freezing is an intersection. Nothing is quantised, so a pool that has had three
spells land in it and an ice shard cut across it is a genuinely irregular outline
rather than a union of discs.

Two rules that are easy to mistake for limitations and are not:

- **A fluid keeps ONE ring; a solid keeps its holes.** Cut a region out of the
  middle of a puddle and the water flows back over it -- so a pool discards holes
  and islands and takes the largest outer ring. A floe with a melted-through gap
  is a floe with a gap you can fall into, so a solid keeps them, SEPARATE rather
  than bridged. Bridging leaves a zero-width slit that the offsetter rounds into
  arcs, and eroding that compounds; the Godot version measured 50 vertices
  becoming 7193 over eighteen melt ticks.
- **Freezing SHRINKS the pool rather than subtracting the frozen region.** The
  fluid is genuinely used up, but a liquid does not keep a hole, so the pool
  keeps its shape and loses the area -- one square root, since area scales with
  the square of a uniform scale.

This also closed the two extension points phase 6 deliberately left open: the
reaction subsystem now asks the fluid system to solidify before resolving an
energy trade, and conduction checks whether a strike was roofed by a solid --
NOT THROUGH THE ICE, since a floe roofs over the water beneath it and the pool's
own collider knows nothing about that.

**A LATER CORRECTION: nothing ever put a puddle in the world.** Phase 10 built
every operation on a body -- deposit, merge, grow, erode, freeze, melt -- and
wired the last two to the reaction and conduction subsystems, but `Deposit`
itself had no caller outside the tests and the melt path. A water spell landed on
dry ground and left it dry, and the same was true one layer down: the fluid
definitions, the solid definitions and the combination table were all
`EditAnywhere` on world subsystems, which have no editing surface, so a real
session ran the whole thing with empty lists. Three things were missing and each
would have been enough on its own to produce no puddles.

- **A cast spell now finishes into the fluid system.** `AARPGDischargeEffect`
  carries a `DepositRadius` and broadcasts a static `OnDischargeLanded` on
  EndPlay(Destroyed); the fluid subsystem subscribes. A STATIC DELEGATE for the
  same reason `OnVolumesMet` is one -- ARPGWorld depends on ARPGMagic, so an
  effect calling the fluid system directly would close the cycle.
- **The ground is probed, not assumed.** A spell finishes at chest height, so the
  landing point is traced down to whatever is under it and the body forms there.
  Past `MaxDepositDrop` the spell expired over a drop and wet nothing, which is an
  outcome rather than a failure.
- **Still nothing in C++ knows that water pools and fire does not.** An element
  with no fluid definition deposits nothing, so the same radius on a fire orb is
  simply inert. What changed is that the miss is now SAYABLE: an element nobody
  pools logs at Verbose, and NOTHING being configured to pool -- the state this
  project was actually in -- warns once and names the setting.
- **The placeholders deposit, so this works with no authored content.** The same
  number that drives a placeholder's hitbox and its drawn volume now drives what
  it leaves, which is the rule that class already lived by. A projectile spreads
  on impact rather than depositing at its own width -- a placeholder orb is 20 to
  60cm and would otherwise leave a wet coin.
- **A deposit under the minimum area no longer becomes a body.** It was spawned,
  replicated, and destroyed by the next weather tick for being under the same
  floor. Refused only where it would have nothing to belong to: a splash too small
  to be a puddle still enlarges one it lands in, and the last of a melting floe
  still returns its water to the pool it froze out of.

3 new cases under `ARPG.World.Fluid.Casting`, the first of which is the gate that
was missing -- a cast spell leaves a body on the ground under where it finished --
plus the area floor asserted in `Pools.DepositsMergeRatherThanStack`.

**A SECOND CORRECTION: a body had no appearance at all.** The phase table above
claimed dynamic meshing and replicated outlines; neither was implemented. A pool
was a trigger box and an elemental volume, `SurfaceMaterial` was declared on both
definitions and read by nothing, and `Ring` was a plain `UPROPERTY` on an actor
that replicates -- so a client received a pool with an empty outline and
`RebuildFromRing` returned early on it.

- **`UDynamicMeshComponent`, not the Water plugin.** Unreal's water bodies are
  spline-authored level geometry served by a water zone: right for a river
  someone placed, wrong for a puddle a spell made half a second ago whose
  outline changes 4Hz. A body here is already a polygon, so drawing it is a
  constrained Delaunay and an extrude -- `ARPGFluidGeometry::BuildSlabMesh`, the
  fifth library call, sitting beside the four the design already reduced to.
- **The mesh is the simulation's ring**, built in `RebuildFromRing` alongside the
  bounds and the volume. There is no second representation to drift. UVs are
  anchored to WORLD position, not the mesh's own space: a pool's centroid moves
  every time it merges or erodes, and local UVs make the surface texture swim
  sideways while the water sits still.
- **The outline is all that replicates.** `Ring`, `GroundHeight`, the definition
  and a solid's hole carry the Net flag and share one rep notify, since they
  arrive in no guaranteed order and a rebuild driven by whichever came first
  would size the mesh against a null definition. No mesh data goes on the wire.
- **A floe is walked on where it is drawn.** Standable solids blocked pawns with
  the BOUNDS BOX -- the polygon's rectangle -- so a player could stand off the
  floe and inside the box, in mid-air over open water. Invisible while nothing
  was drawn, and the first thing you notice once the slab is there. Collision is
  now complex-as-simple on the mesh itself, which is also the only way to keep a
  melted-through hole.
- **Two ordering bugs the mesh exposed.** `TrySolidify` assigned `HoleRing` after
  `Setup`, and the melt tick set the new outline before widening the hole. Both
  were invisible while a hole only fed `IsStandableAt`, which reads it live; both
  now leave a floe drawn and walkable over its own gap.

The materials are placeholders on the same principle as the discharge volumes: a
Fresnel driving opacity, which is most of what makes a flat surface read as
liquid, and no textures at all. A real water shader is still the `M_ocean`
rewrite the plan called for, and still presentation-only -- the waterline stays a
flat number and nothing queries back into it.

3 new cases under `ARPG.World.Fluid.Surface`.

**A THIRD CORRECTION: only a pool could be frozen.** `TrySolidify` required one
side to literally be an `AARPGFluidPool`, which quietly meant only a body this
subsystem had spawned. An authored river -- the case the whole RESERVOIR idea
exists for -- failed the cast and fell through to an ordinary energy trade, so an
ice shard into a river made ice and no floe. And a pool that had grown past
`ReservoirArea` *was* frozen, but was then shrunk by the frozen area and
destroyed once enough had been taken: a body the code had already agreed was
bottomless, depleted by the one path that forgot to ask.

`IARPGFreezableSurface` replaces the cast with the three questions freezing
actually asks -- what shape are you near where I hit you, how high is the surface,
and take this much away. A puddle answers with its ring and shrinks; a reservoir
answers with a patch around the contact and takes nothing. `bReservoir` becomes
one implementation rather than a branch nobody wrote.

**Rivers use the Water plugin, and that is the OPPOSITE call from puddles for a
consistent reason.** The line is not water against not-water; it is AUTHORED AND
STATIC against SPAWNED AND RESHAPED. Water bodies are spline-authored level
geometry served by a water zone, which is exactly a river and exactly not a
puddle. A river is therefore never an `AARPGFluidPool`: it is an
`AWaterBodyRiver` with a `UARPGWaterBodyVolumeComponent` on it, and the fluid
subsystem never hears about it.

Two things the plugin supplies that a box has to guess:

- **Containment.** The base `ContainsPoint` is axis-aligned bounds. For the
  straight box a river was authored as that is roughly honest; for a spline that
  bends it covers the whole valley, so everyone in it reads as standing in water
  -- soaked, shocked, and detonating fireballs over dry ground. Both it and
  `GetSurfaceHeightAt` are now virtual, and the water body answers from the same
  query that drives its own buoyancy.
- **A waterline that varies.** One authored offset cannot describe a river
  running downhill. That was a standing note on `GetSurfaceHeightAt` saying phase
  10 would replace it; phase 10 did not, and this does.

**Freezing is written against containment, not against a shape.** The footprint
is found by marching outward from the contact until the water stops, asking only
`ContainsPoint` -- so `UARPGReservoirVolumeComponent` needs no knowledge of
splines or boxes, a subclass that answers containment better gets a better patch
for free, and the whole path is testable with a plain box and no plugin.

3 new cases under `ARPG.World.Fluid.Reservoir`, including a shard wider than a
narrow river freezing a patch that stops at the bank.

**A FOURTH CORRECTION: nothing could reach the reaction solver, and a reaction
changed nothing about a body.** Two gaps that read as one bug from outside --
elemental collisions simply did not happen in game.

- **A cast spell was not made of anything.** It carried no elemental volume and,
  worse, no primitive collider at all: `UARPGHitboxComponent` is a scene
  component that sweeps by hand, deliberately, because an overlap volume moving
  fast enough passes between frames. Reactions are detected by physics overlap.
  So the whole of phase 6 -- matched projectiles, surplus, lopsided rates, a
  reservoir hissing rather than exploding -- was true, tested, and unreachable.
  `AARPGDischargeEffect` now carries a sphere and a volume, driven by
  `ReactionRadius` and energised from the context's `ComputedDamage`.
- **A collision PRODUCT is the one thing that must not react.** It is born at
  the contact point, inside whichever volume survived, and two different
  elements neutralise even with no recipe -- so a steam cloud would eat the
  fireball's surplus, which is what `SurplusSurvivesAtReducedPower` protects.
- **A reaction now takes GROUND.** Spending a pool's energy used to change
  nothing: the pool recomputes energy from area on the next weather tick and
  silently discarded it, so a fireball into a puddle made steam and left the
  puddle full size. A floe was worse -- it carried no energy at all, so the
  solver bailed at its own zero-energy guard and fire did nothing to ice.
  `IARPGElementalSurface` gains an energy density, and a body converts the spend
  back into area at that rate. The combination table's consumption rates
  therefore already decide how fast fire eats water and ice, with no second set
  of numbers.
- **And it takes ground rather than SCALE.** Without a hook a body fell to the
  volume component's default projectile reaction, which scales the actor by the
  cube root of what is left and destroys it outright at zero -- so a reacting
  pool had its mesh, trigger box and outline disagreeing within a frame, and
  could destroy itself behind the subsystem's back. `AARPGFluidBody` implements
  `IARPGElementalReactive`, and retirement moved into one `RetireBody` that both
  the melt tick and the reaction path go through -- which is also how a floe
  melted by fire returns its water, something only the tick used to do.

The interface is `IARPGElementalSurface` rather than the `IARPGFreezableSurface`
it was one correction ago: a floe is not freezable, but it does have area a
reaction can take, and both callers were asking the same three questions.

4 new cases -- 2 under `ARPG.World.Reaction`, 2 under `ARPG.World.Fluid.Reaction`.

**A FIFTH CORRECTION: a puddle did not conduct.** The conduction subsystem is
sound and four cases prove the chain -- but every one of them sets
`Conductivity` on its volumes by hand, and nothing ever set it on a deposited
pool. The volume's own default is 0, `AARPGFluidBody` never touched it, and
`UARPGFluidDefinition` had no field for it, so there was no authoring surface
either. The conduction filter drops any neighbour at 0, so a chain of real
puddles fell out of its own graph. That is the phase 6 gate -- *lightning floods
a puddle chain and hurts a second player standing in it* -- failing on the half
of itself that phase 6 deferred to phase 10 and phase 10 did not pick up.

It failed in the wrong direction, too. With `Conductivity` at 0 the Conduct
branch is skipped; amplification is correctly excluded for a Conduct row, so the
pair lands in the ordinary energy exchange with the row's `Result` -- Lightning
-- treated as a product. So a bolt into a puddle popped a second lightning
effect, spent both sides, and (once a reaction took ground) boiled some of the
puddle away, while nobody standing in it was shocked. It looked like something
happened.

- `Conductivity` moves onto the fluid definition and is applied in
  `RebuildFromRing`. Which charges a body carries is still a Conduct row in the
  shared table; this is only how well this substance does it.
- A solid's is set to 0 EXPLICITLY. Ice not conducting is the point -- NOT
  THROUGH THE ICE is a rule about the slab roofing the water, and a conductive
  slab would carry the bolt into the pool it floats on and defeat itself.
- **The roofing check widened from reservoirs to any medium.** It was asked only
  when `Medium->bReservoir`, and a pool is only a reservoir past a threshold --
  so ice on an ordinary puddle roofed nothing, which is the common case rather
  than the rare one.

2 new cases under `ARPG.World.Fluid.Conduction`, and they use REAL deposited
pools rather than hand-built volumes, which is the whole reason this survived.

**A floe now FLOATS, and this is a departure from the Godot original rather than
a correction to the port.** It was pinned: `GroundHeight` was set to the waterline
once at creation and never touched, so a floe did not ride waves, did not move on
a current, and did not give under anyone standing on it.

**KINEMATIC, NOT SIMULATED, AND THAT IS THE WHOLE DESIGN.** `UBuoyancyComponent`
is the obvious reach and the wrong one: it drives a SIMULATING rigid body, which
is right for a boat you ride and wrong for a platform you WALK ON -- a simulating
body under a character movement component jitters and gets shoved around by the
character it is carrying. It also requires simple collision, where a floe's whole
value is complex-as-simple collision honouring its exact outline and its
melted-through hole. So the integration is ours and the water state is the
plugin's: surface height and flow velocity come from the same queries that drive
its own buoyancy, and a kinematic movable base is what UE actually carries a
character on.

- **Archimedes, with two knobs traded for feel.** Draft is
  `(SlabMass + LoadMass) / (WaterDensity * Area)`, so the slab's own term reduces
  to `Thickness * Density / WaterDensity` with the area cancelling -- a big floe
  and a small one of the same ice ride equally deep, and a wider floe takes a
  person's weight better. Both fall out rather than being arranged. The two
  departures are named in the definition: real ice at 92% submerged leaves 2cm of
  freeboard on a slab you are meant to walk on, and a real person on 10m² pushes
  it under a centimetre.
- **Drift translates the RING, not the actor**, so the polygon stays the single
  truth. `TranslateRing` skips the mesh rebuild, because a translation changes no
  local geometry -- which is what makes per-frame drift affordable, and which
  also leaves the surface texture riding with the floe instead of swimming past
  it.
- **A floe spanning its water is ANCHORED.** A spell that freezes the whole width
  of a river makes a plug, not a raft: it is braced on both banks. Detected by
  probing for open water past the outline in opposing directions, and recomputed
  as it melts, so one that narrows enough comes free.
- **It reports its velocity**, so based movement carries whoever is standing on
  it. A drifting floe that slid out from under the player would be worse than a
  static one.
- **And it goes when its water goes** -- the mid-air-ice defect that boiling a
  pool away had just made reachable.

4 new cases under `ARPG.World.Fluid.Floating`.

**Phase 11 -- code complete. Not in the original ten: this is the layer that
makes the other ten reachable from a controller.** The modal control scheme, the
speed tiers, the buffering, and auto-sheathe. 13 new cases; 99 pass in total.

The gate is a controller in hand: LT + face readies an element, RT + face
charges and discharges it, a swing spends what is in hand as an imbue, and steel
puts itself away when the fight ends.

**The scheme splits across THREE components rather than living on the pawn**,
which is the one structural departure from Godot's PlayerCharacter and the
reason any of it is testable:

- `UARPGModalInputComponent` (ARPGCore) -- raw actions in, semantic events out,
  zero gameplay knowledge. Runs with no pawn, no controller and no ability
  system, which is how the modal rules get tested at all.
- `UARPGLocomotionComponent` (ARPGCore) -- three speed tiers, the sprint toggle,
  and the stamina that pays for it. Writes `MaxWalkSpeed` and nothing else; the
  movement itself stays with the engine.
- `UARPGPlayerActionComponent` (game module) -- where buttons meet systems, and
  therefore where the element-consumption rule lives. It is deliberately NOT in
  a runtime module: deciding that a swing imbues and a dodge goes elemental is a
  choice BETWEEN the combat and magic systems, so it cannot sit inside either.

Enhanced Input made one thing easier and one thing harder than the Godot
version. Easier: LB's press and release are edges the engine already reports, so
the original's `_process`-time polling of the parry action is gone. Harder: the
original had a real bug where a trigger and a face button pressed on the same
frame routed as a plain attack, because the cached modifier flag was only
refreshed after input was delivered. Two things fix it here -- **the modifiers
are bound FIRST**, since Enhanced Input dispatches a frame's bindings in the
order they were added, and `PollModifiers()` re-reads the live action state at
the moment of the press anyway.

Three rules are preserved exactly, and every one of them looks like a bug from
outside:

- **LT + D-pad Up is swallowed.** Nothing is bound to it and the press is still
  consumed, so being a frame late releasing LT cannot drink a potion.
- **Releasing RT does not cancel a charge.** RT chooses the mode; from there the
  charge belongs to its own face button. The release is checked BEFORE the
  modifiers, so picking up LT mid-charge does not turn the release into an
  element select either.
- **Dodge, block and element select buffer to the cancel window; drinking does
  not.** A potion that arrives several beats late, once the swing finally ends,
  is worse than one that plainly did not happen.

Two corrections to the original:

- **`discharge_cancelled` was declared and never emitted.** A charge begun
  before dying survived the respawn and fired on the first face release
  afterwards. `CancelCharge()` now exists and is what the owner calls.
- **The discharge activates on the PRESS, not the release.** Godot measured the
  charge in the input layer and handed a number over at the end. Here the
  ability owns it, because the charge is not a measurement -- it is a process
  that spends mana server-side while the button is down and forces the release
  when the player runs dry. The input layer still reports a charge fraction, and
  that number is now explicitly a UI value rather than a gameplay one.

Auto-sheathe went onto `UARPGWeaponComponent` rather than the character, since
that component already owns drawn state. Its suppression flag HOLDS the idle
timer at the threshold instead of resetting it -- so the weapon goes away on the
first frame after the last enemy disengages, not a full delay later.

**Still open from this phase**: per-weapon block timings. Godot's
`BlockDefinition` carried its own blend-in and parry window so a buckler and a
greatshield differed; those still come from the parry component's own settings.
That belongs with the rest of the per-weapon animation fields, which are waiting
on content.

**Phase 12 -- code complete. The two things phases 2 and 3 deferred as
presentation, plus the per-weapon data that closes phase 11's open gap.** 7 new
cases; 106 pass in total.

The gate is a hit that READS as a hit: a burning tree alight at its own size, a
heavy blow that catches for a few frames, and a guard whose timing depends on
what is being held.

**Status VFX: one authored asset per effect, fitted to anything.** The naive
shape of this feature is an asset per (effect x silhouette) -- a burning tree, a
burning barrel, a burning rat -- which is an N x M authoring cost that grows
whenever either axis does, and which is wrong about where the variation lives.
What differs between a burning tree and a burning rat is the BOUNDS; what
differs between fire and corruption is the LOOK. Those are orthogonal, so
`ARPGGeometryProbe` measures the first at runtime and only the second is
authored.

Two measurement rules carried over intact, both load-bearing:

- **The probe UNIONS meshes and colliders.** Preferring meshes and falling back
  to colliders looks equivalent and is not: the moment a thing has any mesh its
  collision extent stops counting, and a body of water whose surface is a flat
  plane inside a taller box measures as flat. Reading colliders matters equally
  in the other direction -- a prop whose visual is instanced elsewhere has a
  hull and nothing else.
- **Except for `Top`, which asks a different question.** "Where is this thing's
  surface" is the mesh's top, not the union's; water's collider deliberately
  stands above the waterline so a spell arriving at the river enters it, and an
  arc placed on the union would float in that headroom.

**The subsystem POLLS rather than subscribing, and that is a deliberate
departure.** Godot connected to each combat component's applied/removed/expired
signals to maintain a request map, then reconciled that map on a timer anyway.
Keeping both halves means a missed signal leaves a visual that never retires,
with nothing to correct it. Since the reconcile pass already runs, deriving the
whole map from the ability system's live effect list each pass removes the
bookkeeping AND the class of desync -- at the cost of noticing an affliction up
to `ReevaluateInterval` late, which for a fire lighting is not a cost.

Discovery is one override on `UARPGAbilitySystemComponent`, which everything
with gameplay state already routes through -- so the visual layer costs zero
per-object authoring, the original's best property. It registers on
`InitializeComponent`, NOT BeginPlay: an ability system can carry effects before
play begins, and in a world with no game mode (an automation fixture) BeginPlay
never runs at all.

**Hit-stop got dramatically simpler than the original.** Godot needed an
AnimationTree time-scale sweep plus a separate pose-freeze node to hold the few
clips that had no time scale of their own; `GlobalAnimRateScale` does the whole
job in one number, since the mesh keeps evaluating and applying its pose. The
rule that matters survived unchanged: stacking hits take the LONGEST REMAINING
rather than summing, or a busy exchange piles into a lockup measured in whole
seconds -- and gets worse exactly when the fight gets busiest. It freezes BOTH
parties, never a corpse, and counts down in UNDILATED time so a slow-motion
finisher does not stretch every hit-stop inside it into a stall.

**Per-weapon block and flinch data closes the gap phase 11 left open.** A
buckler snaps up and offers a narrow parry; a greatshield takes a beat and then
covers everything -- and that is a property of the weapon, not of the character.
`UARPGBlockDefinition` also carries the movement scale a raised guard costs, and
dropping the guard gives it back. A weapon with no block authored still defends
on the parry component's own settings, so an unfinished weapon is playable
rather than defenceless.

Two things this phase surfaced that were worth fixing rather than working
around: a VFX actor with no root component silently no-opped through every
placement call and would have sat at the world origin at unit size looking like
a fitting bug, so the probe now refuses it loudly; and automation worlds have no
game mode, which means **no BeginPlay runs in them at all** -- worth knowing
before writing another fixture that assumes it does.

**Placeholder magic visuals -- done, as a follow-on to phase 12 rather than a
phase of its own.** 5 new cases; 111 pass in total.

Phase 5 ported the palette, `ResolveDischargeEffect` and the settings that name
the fallbacks -- but nothing built the fallbacks, nothing handed the palette to
a spawned effect, and nothing ever called `ResolveHandEffect`. The palette was a
data asset nothing read, so every unauthored element was invisible.

**The tint arrives from the SPAWN POINT**, alongside where the hitbox is
stamped, and for the same reason: an effect asset should be art, not a place
that re-reads the element. `IARPGElementTintable` is optional in the way
`IARPGVfxFittable` is -- an authored fire effect that is already orange simply
does not implement it. What it buys is that one asset can serve every element,
which is what makes the placeholders work at all.

**The placeholder is a debug volume, not a VFX, and the drawn shape and the
hitbox are the same number.** What you see is literally what the attack hits.
Particles and bloom would hide the one thing a placeholder is for -- reading
reach and shape while tuning -- and a placeholder that looked finished would
also stop anyone replacing it. It needs no authored content whatsoever: no mesh,
no material, no particle system. A fallback that itself had to be authored would
not be a fallback.

Two departures from the Godot original, both because UE's hitbox sweeps a sphere
between frames rather than owning a shape:

- **A burst TRAVELS instead of being drawn as a static capsule.** Godot gave it
  a capsule collider and a matching capsule mesh; here a burst is honestly a
  sphere thrown forward, and moving it makes the drawn volume exact at every
  instant rather than an approximation of a shape the hitbox cannot express.
- **The volume is debug-drawn** rather than being a translucent mesh, since
  nothing in the engine ships a translucent material to lean on and authoring
  one would defeat the purpose. The tinted light that goes with it is real, so
  something still shows in a packaged build.

`UARPGHandVisualComponent` closes the last gap: LT readying an element was
completely invisible, which made everything downstream -- a swing imbuing, a
dodge going elemental, RT discharging what is in hand -- impossible to verify by
eye. It tracks the DISPLAY element rather than the raw selection, so fire plus
water shows the steam they resolve to; a combination is one thing, and it is the
thing about to be cast.

All four settings slots default to the built-in stand-ins, so magic is visible
in a fresh checkout with no configuration. What is still owed is content that
was always going to be: the element data assets themselves, each with a palette.
An element with no palette stays invisible on purpose -- that is the documented
opt-out, not a missing asset.

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
| 5 ✅ | Magic: elements, loadout pages, combination table, complexity gating, all discharge types, imbue, elemental dodge, cloak | fire+water→steam; cast, imbue, dodge all work; charge drain is server-clamped |
| 6 ✅ | `ElementalVolume`, reaction subsystem, conduction subsystem | Fireball into water jet produces steam at the right contact point; lightning floods a puddle chain and hurts a *second player* standing in it |
| 7 ✅ | Progression trackers, inventory, quick slots, consumables | Mastery multiplies discharge damage; quick-slot potions work |
| 8 ✅ | NPC AI — perception, behavior trees, reactive parry | NPC engages, parries a telegraphed swing, flees and heals |
| 9 ✅ | Fire spread subsystem (+ landscape fuel bake, MPC mask, replicated mask) | Grass fire crosses a clearing, burns a second player, rain quenches it |
| 10 ✅ | Fluid surface subsystem (+ Clipper2 backend, dynamic meshing, replicated outlines) | Water pools; ice shard freezes a floe both players can stand on |
| 11 ✅ | Modal input scheme, speed tiers and sprint stamina, input buffering, element-consumption routing, auto-sheathe | Controller in hand: LT+face readies, RT+face discharges, a swing imbues, steel sheathes itself |
| 12 ✅ | Fitted status VFX + budget, hit-stop, per-weapon block and flinch definitions | A burning tree is alight at its own size; a heavy blow catches; a greatshield guards differently from a buckler |

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
