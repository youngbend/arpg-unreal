// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "ARPGSwingAugment.h"
#include "ARPGGameplayAbility_Imbue.generated.h"

class AActor;
class UARPGMagicElement;

/**
 * Spends the readied element to coat the weapon for its next attack. Port of
 * Godot's consume_for_imbue plus the imbue scene handling.
 *
 * IMBUE IS NOT A DISCHARGE TYPE'S POORER COUSIN. It pays a FLAT cost and has no
 * charge, because there is nothing to charge: the swing's own motion value is
 * what scales it, and that is the attack's property rather than the spell's.
 * That is also why it can use an ordinary one-shot cost path while the four
 * chargeable types cannot.
 *
 * THE ELEMENTAL HIT IS SEPARATE FROM THE WEAPON'S. The weapon deals its own
 * physical damage and the imbue deals elemental damage on the same swing, as two
 * independent hits. Folding them into one would force a single damage type
 * through the mitigation pipeline and lose exactly the interaction that makes
 * imbuing worthwhile -- armour resists the steel, resistance resists the fire,
 * and they are not the same number. Separate MITIGATION, one contact: the
 * payload rides the weapon's own hitbox as an FARPGElementalRider, which is
 * documented there and is not the same thing as a second hitbox.
 *
 * THE SWING DRIVES THIS, NOT THE OTHER WAY AROUND. The ability holds the coating
 * and waits; UARPGGameplayAbility_MeleeAttack stamps it onto each hitbox window
 * and tells it when the attack ended, through IARPGSwingAugment. Combat cannot
 * name anything in ARPGMagic -- see that interface for why it has to be this way
 * round.
 *
 * ONE COATING AT A TIME. InstancedPerActor and not retriggerable, so a second
 * press while a coating is already held is refused: the readied element stays
 * readied and its mana is not spent, rather than silently replacing a coating
 * the player has already paid for.
 */
UCLASS()
class ARPGMAGIC_API UARPGGameplayAbility_Imbue : public UGameplayAbility, public IARPGSwingAugment
{
	GENERATED_BODY()

public:
	UARPGGameplayAbility_Imbue();

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility, bool bWasCancelled) override;

	/**
	 * How long the imbue stays readied without being spent on a swing. 0 keeps it
	 * until the next attack, however long that takes.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Imbue",
		meta = (ClampMin = "0.0"))
	float ImbueDuration = 0.f;

	UFUNCTION(BlueprintPure, Category = "ARPG|Imbue")
	UARPGMagicElement* GetImbuedElement() const { return ImbuedElement; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Imbue")
	bool IsImbued() const { return ImbuedElement != nullptr; }

	/**
	 * Stamps the imbue's elemental payload onto a hitbox as a rider, for one
	 * window, scaled by that window's motion value. Returns false when there is
	 * nothing to stamp.
	 *
	 * A RIDER, NOT A REWRITE. The hitbox's own BaseDamage, DamageType and
	 * OnHitEffects are the WEAPON's and are left exactly as the attack armed
	 * them -- writing the element over them would replace the physical hit
	 * instead of accompanying it, which is the opposite of the point.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Imbue")
	bool ApplyToHitbox(class UARPGHitboxComponent* Hitbox, float MotionValue);

	/** Spends the imbue and ends the ability, dropping the coating and its VFX. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Imbue")
	void ConsumeImbue();

	// --- IARPGSwingAugment ----------------------------------------------------

	virtual void ArmSwingAugment_Implementation(UARPGHitboxComponent* Hitbox,
		float MotionValue) override;
	virtual void NotifySwingEnded_Implementation() override;

private:
	void SpawnImbueEffect();

	UPROPERTY(Transient)
	TObjectPtr<UARPGMagicElement> ImbuedElement;

	UPROPERTY(Transient)
	TObjectPtr<AActor> ImbueEffect;

	/**
	 * Whether the coating actually reached a live hitbox window.
	 *
	 * What makes the imbue spendable. A press that got buffered, an attack
	 * cancelled during its wind-up, or a beat authored with no hit at all all end
	 * a swing without ever arming -- and the player has already paid the mana, so
	 * the coating has to survive for the swing that does land. Refusing to
	 * deliver it AND taking it would charge them twice.
	 */
	bool bDelivered = false;

	FTimerHandle ExpiryTimer;
};
