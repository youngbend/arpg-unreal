// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
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
 * THE ELEMENTAL HITBOX IS SEPARATE FROM THE WEAPON'S. The weapon deals its own
 * physical damage and the imbue deals elemental damage on the same swing, as two
 * independent hits. Folding them into one would force a single damage type
 * through the mitigation pipeline and lose exactly the interaction that makes
 * imbuing worthwhile -- armour resists the steel, resistance resists the fire,
 * and they are not the same number.
 */
UCLASS()
class ARPGMAGIC_API UARPGGameplayAbility_Imbue : public UGameplayAbility
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
	 * Stamps the imbue's elemental payload onto a hitbox for one swing. Called by
	 * the melee ability when it arms, with that swing's motion value.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Imbue")
	void ApplyToHitbox(class UARPGHitboxComponent* Hitbox, float MotionValue);

	/** Spends the imbue. Called once the swing carrying it has landed. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Imbue")
	void ConsumeImbue();

private:
	void SpawnImbueEffect();

	UPROPERTY(Transient)
	TObjectPtr<UARPGMagicElement> ImbuedElement;

	UPROPERTY(Transient)
	TObjectPtr<AActor> ImbueEffect;

	FTimerHandle ExpiryTimer;
};
