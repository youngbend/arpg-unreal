// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
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
class ARPGCOMBAT_API UARPGWeaponComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGWeaponComponent();

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Equipped on begin play. The character's starting kit. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|Weapon")
	TObjectPtr<UARPGWeaponDefinition> DefaultWeapon;

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

protected:
	UFUNCTION()
	void OnRep_Weapon();

	UFUNCTION()
	void OnRep_Drawn();

private:
	/** Pushes the weapon's tree and damage onto the combo component and hitbox. */
	void ApplyWeaponToOwner();

	/**
	 * Server-only. Moves the weapon's crit bonus onto the wielder's CritChance
	 * attribute, removing exactly what the previous weapon added.
	 *
	 * The bonus goes on the ATTRIBUTE rather than onto the hitbox's own
	 * CriticalChance, because the hitbox value is authored per attack -- an
	 * execution-style finisher may be flatly more likely to crit -- and writing
	 * the weapon's bonus there would overwrite that authored intent. As an
	 * attribute it also composes with buffs through the usual aggregator.
	 */
	void RefreshCritChance();

	UAbilitySystemComponent* GetASC() const;
	UARPGComboComponent* GetCombo() const;
	UARPGHitboxComponent* GetWeaponHitbox() const;

	UPROPERTY(ReplicatedUsing = OnRep_Weapon)
	TObjectPtr<UARPGWeaponDefinition> Weapon;

	UPROPERTY(ReplicatedUsing = OnRep_Drawn)
	bool bDrawn = false;

	/** What this component last added to CritChance, so swapping reverses it exactly. */
	float AppliedCritChance = 0.f;

	UPROPERTY(Transient)
	mutable TObjectPtr<UAbilitySystemComponent> CachedASC;
};
