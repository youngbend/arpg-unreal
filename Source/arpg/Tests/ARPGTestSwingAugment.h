// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "ARPGSwingAugment.h"
#include "ARPGTestSwingAugment.generated.h"

class UARPGAttackDefinition;
class UARPGHitboxComponent;

/**
 * A stand-in for the imbue, so the combo component's augment handling can be
 * tested without one.
 *
 * The real augment is UARPGGameplayAbility_Imbue, which needs a magic component,
 * a readied element, mana and an elemental hitbox before it will answer anything
 * -- none of which the combo state machine cares about. What it needs from an
 * augment is one question answered, so this answers it and nothing else.
 *
 * Test-only, and in the Tests folder for the same reason ARPGTestVfxActor is.
 */
UCLASS()
class UARPGTestSwingAugment : public UGameplayAbility, public IARPGSwingAugment
{
	GENERATED_BODY()

public:
	UARPGTestSwingAugment();

	/** Stays active once activated, which is what makes FindActive see it. */
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual UARPGAttackDefinition* GetSwingAttackOverride_Implementation(
		EARPGAttackInput Input) override;
	virtual void ArmSwingAugment_Implementation(UARPGHitboxComponent* SwingHitbox,
		float MotionValue) override;
	virtual void DisarmSwingAugment_Implementation() override;
	virtual void NotifySwingEnded_Implementation() override;

	/** Answered from GetSwingAttackOverride. Null entries mean "no override". */
	UPROPERTY(Transient)
	TObjectPtr<UARPGAttackDefinition> OverrideLight;

	UPROPERTY(Transient)
	TObjectPtr<UARPGAttackDefinition> OverrideHeavy;

	UPROPERTY(Transient)
	TObjectPtr<UARPGAttackDefinition> OverrideSpecial;

	/** Observations, so a test can assert the swing drove this correctly. */
	int32 ArmCount = 0;
	int32 DisarmCount = 0;
	int32 SwingEndedCount = 0;
	float LastMotionValue = 0.f;
};
