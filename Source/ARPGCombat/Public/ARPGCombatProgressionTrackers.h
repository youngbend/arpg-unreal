// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGProgressionTrackerComponent.h"
#include "ARPGCombatProgressionTrackers.generated.h"

class UARPGHitboxComponent;

/**
 * Weapon proficiency. Port of Godot's WeaponProgressionTracker.
 *
 * XP COMES FROM LANDING HITS, not from swinging. Skill with a sword is about
 * connecting with it; the same is deliberately NOT true of magic, where casting
 * alone counts -- see UARPGMagicProgressionTracker.
 *
 * The subcomponent is the equipped weapon's TYPE rather than the specific
 * weapon, so a player who has mastered swords does not start over when they
 * find a better one.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCOMBAT_API UARPGWeaponProgressionTracker : public UARPGProgressionTrackerComponent
{
	GENERATED_BODY()

public:
	/** Multiplies damage XP into progression XP. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Progression",
		meta = (ClampMin = "0.0"))
	float XPPerDamage = 1.f;

protected:
	virtual void BindXPSource() override;

	UFUNCTION()
	void HandleHitLanded(AActor* HitActor, const FHitResult& Hit, float Damage);
};

/**
 * Armour proficiency. Port of Godot's ArmorProgressionTracker.
 *
 * XP COMES FROM BEING HIT, which is the only thing armour does. That makes it
 * the one tracker whose progression the player earns by failing to dodge, and
 * the reason its multiplier feeds mitigation rather than damage.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCOMBAT_API UARPGArmorProgressionTracker : public UARPGProgressionTrackerComponent
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Progression",
		meta = (ClampMin = "0.0"))
	float XPPerDamage = 1.f;

protected:
	virtual void BindXPSource() override;

	UFUNCTION()
	void HandleHitReceived(const FGameplayEffectContextHandle& Context, float Magnitude);
};
