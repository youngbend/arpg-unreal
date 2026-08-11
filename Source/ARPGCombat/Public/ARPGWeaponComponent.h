// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGGameplayComponentBase.h"
#include "GameplayEffectTypes.h"
#include "ARPGWeaponComponent.generated.h"

class UARPGWeaponDefinition;
class UARPGComboComponent;
class UARPGHitboxComponent;
class UAbilitySystemComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnWeaponChanged, UARPGWeaponDefinition*, Weapon);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnWeaponDrawnChanged, bool, bDrawn);

/**
 * Holds the equipped weapon. Port of Godot's WeaponComponent.
 *
 * EQUIPPING REWIRES THE MOVESET. On equip this pushes the weapon's attack tree
 * onto the sibling combo component and its base damage onto the weapon hitbox,
 * so a greatsword genuinely changes what every attack button does. Before this
 * existed the character carried a hard-coded tree path, which broke silently the
 * first time the asset moved -- a weapon's moveset belongs to the weapon.
 *
 * Sheathe state is tracked but purely advisory here: the visual reparenting
 * between hand and sheath sockets belongs to the animation layer, which listens
 * to OnWeaponDrawnChanged.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCOMBAT_API UARPGWeaponComponent : public UARPGGameplayComponentBase
{
	GENERATED_BODY()

public:
	UARPGWeaponComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Equipped on begin play. The character's starting kit. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|Weapon")
	TObjectPtr<UARPGWeaponDefinition> DefaultWeapon;

	/**
	 * What the weapon hitbox is worth with nothing equipped -- fists, and only
	 * fists.
	 *
	 * Zero by default, because an unarmed moveset that hurts is a design choice
	 * and a hitbox left at its authored WeaponBaseDamage is an accident. This is
	 * the number the combo component's FallbackAttackTree swings for; authoring
	 * one without setting this gives a punch that connects and does nothing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Weapon",
		meta = (ClampMin = "0.0"))
	float UnarmedBaseDamage = 0.f;

	UFUNCTION(BlueprintCallable, Category = "ARPG|Weapon")
	void EquipWeapon(UARPGWeaponDefinition* NewWeapon);

	UFUNCTION(BlueprintCallable, Category = "ARPG|Weapon")
	void UnequipWeapon();

	UFUNCTION(BlueprintPure, Category = "ARPG|Weapon")
	UARPGWeaponDefinition* GetWeapon() const { return Weapon; }

	/** Sheathed by default: a character does not start with steel drawn. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Weapon")
	void SetDrawn(bool bNewDrawn);

	UFUNCTION(BlueprintPure, Category = "ARPG|Weapon")
	bool IsDrawn() const { return bDrawn; }

	/**
	 * Swing damage, computed at SWING time rather than on the defender at
	 * hit-resolution -- matching how magic bakes its multipliers in at cast time.
	 * Folds the weapon's own formula together with the wielder's outgoing
	 * damage-amp, so a Weakened status weakens swings exactly as it weakens
	 * spells.
	 */
	UFUNCTION(BlueprintPure, Category = "ARPG|Weapon")
	float GetEffectiveDamage(float MotionValue) const;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Weapon")
	FARPGOnWeaponChanged OnWeaponChanged;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Weapon")
	FARPGOnWeaponDrawnChanged OnWeaponDrawnChanged;

	// --- Auto-sheathe ---------------------------------------------------------
	//
	// Steel goes away on its own once the fight does, so the player never has to
	// think about putting it back. Lives here rather than on the character
	// because this component already owns drawn state -- and because a thing
	// that puts a weapon away is a weapon concern.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Weapon|Auto Sheathe")
	bool bAutoSheatheEnabled = true;

	/** Seconds of no combat activity before the weapon goes away by itself. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Weapon|Auto Sheathe",
		meta = (ClampMin = "0.0"))
	float AutoSheatheDelay = 5.f;

	/**
	 * Restarts the idle countdown. Called on every attack, block and hit taken.
	 *
	 * A method rather than a set of delegate bindings: the events that count as
	 * "still fighting" are not all combat events, and the owner is the only
	 * thing that knows which ones its own scheme cares about.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Weapon|Auto Sheathe")
	void NotifyCombatActivity();

	/**
	 * Holds off the sheathe without resetting the countdown -- for as long as
	 * something out there is still aiming at you.
	 *
	 * The distinction from NotifyCombatActivity is the whole point. Resetting
	 * while suppressed would mean waiting out another full delay after the last
	 * enemy disengages; holding AT the threshold means the weapon goes away on
	 * the very next frame, which is what "the fight is over" should feel like.
	 *
	 * Set by the owner: knowing whether an NPC is targeting you needs the AI
	 * layer, which sits above this module.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Weapon|Auto Sheathe")
	void SetAutoSheatheSuppressed(bool bSuppressed) { bAutoSheatheSuppressed = bSuppressed; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Weapon|Auto Sheathe")
	float GetTimeSinceCombatActivity() const { return TimeSinceCombatActivity; }

protected:
	UFUNCTION()
	void OnRep_Weapon();

	UFUNCTION()
	void OnRep_Drawn();

private:
	/** Pushes the weapon's tree and damage onto the combo component and hitbox. */
	void ApplyWeaponToOwner();

	/**
	 * Server-only. Grants the weapon's crit bonus on the wielder's CritChance
	 * attribute for as long as it is wielded.
	 *
	 * The bonus goes on the ATTRIBUTE rather than onto the hitbox's own
	 * CriticalChance, because the hitbox value is authored per attack -- an
	 * execution-style finisher may be flatly more likely to crit -- and writing
	 * the weapon's bonus there would overwrite that authored intent. As an
	 * attribute it also composes with buffs through the usual aggregator.
	 *
	 * A GameplayEffect rather than a write to the base value, for the reasons in
	 * UARPGEquipmentGameplayEffect: the previous version read the CURRENT crit
	 * chance and wrote it back as the BASE, so swapping weapons while any crit
	 * buff was live baked that buff in permanently.
	 */
	void RefreshCritChance();

	UARPGComboComponent* GetCombo() const;
	UARPGHitboxComponent* GetWeaponHitbox() const;

	UPROPERTY(ReplicatedUsing = OnRep_Weapon)
	TObjectPtr<UARPGWeaponDefinition> Weapon;

	UPROPERTY(ReplicatedUsing = OnRep_Drawn)
	bool bDrawn = false;

	/** The live crit grant, removed exactly on swap or unequip. */
	FActiveGameplayEffectHandle CritGrantHandle;

	/** Server-authoritative: bDrawn replicates, so only authority may run the countdown. */
	void TickAutoSheathe(float DeltaTime);

	float TimeSinceCombatActivity = 0.f;
	bool bAutoSheatheSuppressed = false;
};
