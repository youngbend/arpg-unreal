// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "ARPGCombatTypes.h"
#include "ARPGAttackDefinition.generated.h"

class UAnimMontage;
class UARPGDamageTypeAsset;
class UGameplayEffect;

/**
 * What the .tres carried that a montage now owns.
 *
 * Kept on the asset because it is the montage-authoring checklist: which clip
 * belongs in which section, and where the combo cancel notify goes. Without it
 * that information lives only in the Godot repo, and the person building the
 * montage has to go and find it.
 *
 * Editor-only, and never read at runtime. Writable rather than display-only
 * because the converter sets it through the Python editor API, which cannot
 * touch VisibleAnywhere properties -- and because hand-correcting a clip name
 * is a reasonable thing to want to do.
 */
USTRUCT()
struct FARPGAttackImportData
{
	GENERATED_BODY()

	/** Source .tres this asset was converted from. */
	UPROPERTY(EditAnywhere, Category = "Import")
	FString SourceFile;

	UPROPERTY(EditAnywhere, Category = "Import|Montage Sections")
	FString WindupClip;

	UPROPERTY(EditAnywhere, Category = "Import|Montage Sections")
	FString ActiveClip;

	UPROPERTY(EditAnywhere, Category = "Import|Montage Sections")
	FString GapClip;

	UPROPERTY(EditAnywhere, Category = "Import|Montage Sections")
	FString Active2Clip;

	UPROPERTY(EditAnywhere, Category = "Import|Montage Sections")
	FString LandingClip;

	UPROPERTY(EditAnywhere, Category = "Import|Montage Sections")
	FString RecoveryClip;

	/**
	 * Seconds into the Recovery section at which the combo cancel window opened.
	 *
	 * No longer a runtime field -- it is now the POSITION of the
	 * Event.Attack.ComboWindow notify within the montage. Carried across so the
	 * authored timing is not quietly lost and re-guessed.
	 */
	UPROPERTY(EditAnywhere, Category = "Import")
	float RecoveryCancelDelay = 0.f;
};

/**
 * Static data for one attack in a combo tree. Port of Godot's AttackDefinition.
 *
 * THE SIX ANIMATION NAME FIELDS ARE GONE. Godot stored clip NAMES for windup,
 * active, gap, active2, landing and recovery, and animation.gd sequenced them by
 * hand -- roughly a third of that 3,900-line file existed to do what a montage
 * does natively. Here one montage carries all of them as SECTIONS, and the phase
 * boundaries are anim notifies rather than elapsed-time bookkeeping.
 *
 * That also means clip LENGTHS are now knowable. The Godot ComboComponent
 * carried time_until_hit_estimate, windup_seconds_remaining and
 * active_phase_duration purely because AttackDefinition stored names and had no
 * access to durations; animation.gd had to publish them every frame for the
 * reactive-parry AI. A montage knows its own section times, so that whole
 * channel disappears in phase 8.
 */
UCLASS(BlueprintType)
class ARPGCOMBAT_API UARPGAttackDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	// --- Identity -------------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FName AttackId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FText DisplayName;

	// --- Animation ------------------------------------------------------------

	/**
	 * Sections are expected to be named Windup / Active / Gap / Active2 /
	 * Landing / Recovery. Only Windup and Active are required.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Animation")
	TObjectPtr<UAnimMontage> Montage;

	/**
	 * Overlay the attack on the upper body, letting the legs keep locomoting.
	 * False plays it full-body (spinning slashes, committed heavies).
	 * Selects which montage slot is used.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Animation")
	bool bBlendLocomotion = true;

	/** Multiplied against run speed while the attack is active. 0 = rooted. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Animation",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MovementSpeedFactor = 1.f;

	// --- Damage ---------------------------------------------------------------

	/** Damage scalar for the first active window. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage",
		meta = (ClampMin = "0.0"))
	float MotionValue = 1.f;

	/** Damage scalar for the second active window, when the montage has one. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage",
		meta = (ClampMin = "0.0"))
	float MotionValue2 = 1.f;

	/** Damage scalar for a landing impact. 0 = no hitbox on landing at all. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage",
		meta = (ClampMin = "0.0"))
	float LandingMotionValue = 0.f;

	/** Seconds between re-hits of a still-overlapping target. 0 = one hit per activation. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage",
		meta = (ClampMin = "0.0"))
	float HitboxTickInterval = 0.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
	EARPGHitboxSource HitboxSource = EARPGHitboxSource::Weapon;

	/**
	 * Widens this attack's hitbox for the duration of its windows, as a multiple
	 * of the hitbox's authored TraceRadius.
	 *
	 * REACH MOSTLY COMES FROM THE MONTAGE, not from here. The hitbox is attached
	 * to the weapon socket and sweeps between frames, so an animation that sends
	 * the blade out on its own already extends the attack's reach for free, and
	 * the sweep cannot tunnel past anything on the way. This is for the part
	 * animation cannot express: a swing that should connect loosely rather than
	 * on the edge, because the thing swinging is a floating blade in a magnetic
	 * field rather than a held one.
	 *
	 * Restored when the window closes, like DamageTypeOverride.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage",
		meta = (ClampMin = "0.01"))
	float HitboxRadiusScale = 1.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
	TObjectPtr<UARPGDamageTypeAsset> DamageTypeOverride;

	/**
	 * How hard this pushes what it hits. NEGATIVE PULLS: the knockback line runs
	 * from the hitbox out to the contact point, so a negative power drags the
	 * target back along it instead -- which is how a magnetic yank is authored.
	 *
	 * The clamp used to floor this at zero, back when nothing read the value at
	 * all. See UARPGCombatLibrary::ApplyKnockback.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
	float KnockbackPower = 100.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage",
		meta = (ClampMin = "0.0"))
	float PoiseDamage = 1.f;

	/**
	 * When true, PoiseDamage is a coefficient scaled by the swing's effective
	 * motion value, so charge scaling carries over for free and big hits build
	 * more stagger. False makes it an absolute value -- for a weak hit that
	 * should still stagger hard, or the reverse.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
	bool bPoiseScalesWithMotion = true;

	/** Seconds of animation freeze on both parties when this connects. 0 = none. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage",
		meta = (ClampMin = "0.0"))
	float HitStopDuration = 0.f;

	/**
	 * Immune to being interrupted by incoming poise damage while performing this
	 * attack. A stance break can still land -- see UARPGPoiseComponent.
	 * Grants State.Hyperarmor for the attack's duration.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
	bool bHyperarmor = false;

	/** Cannot be blocked or parried; must be dodged. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
	bool bUnblockable = false;

	/**
	 * This attack can connect within the first frames of its active window --
	 * the weapon already overlaps the target when the hitbox opens.
	 *
	 * Authored rather than detected because no runtime signal distinguishes
	 * "hits instantly" from "swings through an arc first" until it is too late
	 * to react. Reactive AI uses this to raise guard during the WINDUP instead.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
	bool bEarlyContact = false;

	/** Elemental coatings apply to this attack when the player has one readied. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
	bool bCanBeElemental = true;

	/** Applied to whatever this attack hits. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
	TArray<TSubclassOf<UGameplayEffect>> OnHitEffects;

	// --- Combo ----------------------------------------------------------------

	/**
	 * Seconds after this attack's cancel window opens during which a brand-new
	 * combo cannot start. Does NOT gate dodge, parry or hitstun interrupts, nor
	 * mid-chain follow-ups -- only re-entry to root.
	 *
	 * Only takes effect where this attack sits on a LEAF node. The same
	 * definition can be a leaf in one branch and mid-chain in another, so
	 * finisher-ness is a position in the tree, not a property of the attack.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo",
		meta = (ClampMin = "0.0"))
	float FinisherLockout = 0.f;

	/**
	 * Extra motion value for each combo beat that came before this attack, as a
	 * fraction. 0.25 with three preceding beats swings for 1.75x.
	 *
	 * WHAT THIS IS FOR. An attack that ENDS the chain -- an element's own move,
	 * or an authored finisher -- otherwise costs the same whenever it is thrown,
	 * which means the cheapest moment to use it is from neutral, where there is
	 * no chain to lose. That is backwards: the interesting decision is whether to
	 * cash in now or push for one more beat, and it only exists if the payout
	 * grows with what is being spent.
	 *
	 * Off by default, so no existing attack changes. It is not restricted to
	 * chain-enders either -- a mid-chain beat may be authored to reward depth
	 * too, and nothing here needs to know the difference.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo",
		meta = (ClampMin = "0.0"))
	float ChainDepthBonus = 0.f;

	/**
	 * Beats past which the bonus stops growing, so a long chain cannot be farmed
	 * into an unbounded hit.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo",
		meta = (ClampMin = "0", EditCondition = "ChainDepthBonus > 0.0"))
	int32 MaxChainDepth = 3;

	/**
	 * Holding the input keeps the chain going automatically once this attack
	 * finishes, with no fresh press. Each beat is still its own attack with its
	 * own cost and hitbox activation.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
	bool bContinuousHold = false;

	/**
	 * Marks this as a finisher. Purely informational -- the combo system reads
	 * tree position, not this flag. Use it to trigger kill reactions and VFX.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combo")
	bool bIsFinisher = false;

	// --- Charging -------------------------------------------------------------

	/**
	 * The Windup section is time-stretched so it takes ChargeTime to complete.
	 * Releasing early jumps straight to Active at the fraction reached.
	 * Mutually exclusive with bChannel.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Charging")
	bool bChargeable = false;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Charging",
		meta = (EditCondition = "bChargeable", ClampMin = "0.0"))
	float ChargeMotionMin = 0.8f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Charging",
		meta = (EditCondition = "bChargeable", ClampMin = "0.0"))
	float ChargeMotionMax = 2.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Charging",
		meta = (EditCondition = "bChargeable", ClampMin = "0.01"))
	float ChargeTime = 1.f;

	// --- Channel --------------------------------------------------------------

	/**
	 * Hold-to-channel, e.g. a drill. Windup plays once, then Active loops while
	 * held and stamina lasts. Mutually exclusive with bChargeable.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Channel")
	bool bChannel = false;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Channel",
		meta = (EditCondition = "bChannel", ClampMin = "0.0"))
	float ChannelStaminaPerSecond = 0.f;

	// --- Movement -------------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement")
	float ForwardImpulse = 0.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement",
		meta = (ClampMin = "0.0"))
	float TravelDistance = 0.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement",
		meta = (ClampMin = "0.0"))
	float TravelChargeScale = 0.f;

	/** Fraction of the active window over which the travel burst is applied. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float TravelPhase = 0.35f;

	// --- Aim ------------------------------------------------------------------

	/**
	 * How much of the solved spine-pitch correction this attack takes, for
	 * reaching targets of a different height. 0 opts out entirely -- a
	 * deliberate ankle sweep or an already-downward plunge.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Aim",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SwingPitchScale = 1.f;

	/** Ceiling on that correction. Past ~20 degrees the torso reads as bending, not aiming. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Aim",
		meta = (ClampMin = "0.0", ClampMax = "60.0"))
	float MaxSwingPitchDeg = 20.f;

	// --- Cost -----------------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Cost",
		meta = (ClampMin = "0.0"))
	float StaminaCost = 10.f;

	// --- Derived --------------------------------------------------------------

	/** Effective motion value for a normalised charge. Returns MotionValue when not chargeable. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Attack")
	float GetChargedMotionValue(float Charge) const
	{
		return bChargeable
			? FMath::Lerp(ChargeMotionMin, ChargeMotionMax, FMath::Clamp(Charge, 0.f, 1.f))
			: MotionValue;
	}

	/**
	 * The same motion value after the chain-depth payoff. Applied on top of
	 * whatever the window resolved to, charge included.
	 *
	 * Poise follows through GetEffectivePoiseDamage where the attack scales its
	 * poise with motion -- a finisher earned by a long chain should stagger
	 * harder as well as hurt more, and one authored with absolute poise opts out.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Attack")
	float GetChainScaledMotionValue(float InMotionValue, int32 ChainDepth) const
	{
		if (ChainDepthBonus <= 0.f || ChainDepth <= 0)
		{
			return InMotionValue;
		}

		const int32 Counted = FMath::Min(ChainDepth, FMath::Max(0, MaxChainDepth));
		return InMotionValue * (1.f + ChainDepthBonus * Counted);
	}

	/** Poise damage for a swing at the given effective motion value. */
	UFUNCTION(BlueprintPure, Category = "ARPG|Attack")
	float GetEffectivePoiseDamage(float InMotionValue) const
	{
		return bPoiseScalesWithMotion ? PoiseDamage * InMotionValue : PoiseDamage;
	}

	// --- Import provenance ----------------------------------------------------

#if WITH_EDITORONLY_DATA
	UPROPERTY(EditAnywhere, Category = "Import")
	FARPGAttackImportData ImportData;
#endif

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId("ARPGAttack", GetFName());
	}
};
