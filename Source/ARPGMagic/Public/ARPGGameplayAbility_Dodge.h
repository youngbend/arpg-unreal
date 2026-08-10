// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "ARPGGameplayAbility_Dodge.generated.h"

class AActor;
class UARPGElementalDodgeData;
class UARPGMagicElement;
class UARPGStandardDodgeData;

/**
 * The dodge, elemental or not. Port of Godot's dodge handling plus
 * consume_for_dodge.
 *
 * ONE ABILITY FOR BOTH, because from the player's point of view it IS one
 * button. Holding an element changes what the dodge is, not which action they
 * took -- and splitting it in two would mean two abilities racing to answer the
 * same press, with the loser's i-frames silently not applying.
 *
 * The element is consumed FIRST and its data then drives everything: distance,
 * duration, i-frames, the impulse, the montage and the damage. An element with
 * no ElementalDodge authored falls back to the standard dodge rather than
 * producing a broken elemental one -- see UARPGMagicElement::ElementalDodge.
 */
UCLASS()
class ARPGMAGIC_API UARPGGameplayAbility_Dodge : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UARPGGameplayAbility_Dodge();

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility, bool bWasCancelled) override;

	/** Used when no element is readied, or the readied one has no dodge of its own. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Dodge")
	TObjectPtr<UARPGStandardDodgeData> StandardDodge;

	UFUNCTION(BlueprintPure, Category = "ARPG|Dodge")
	UARPGMagicElement* GetDodgeElement() const { return DodgeElement; }

protected:
	UFUNCTION()
	void OnDodgeFinished();

	UFUNCTION()
	void OnInvincibilityStart();

	UFUNCTION()
	void OnInvincibilityEnd();

private:
	/** Launches the character and applies the elemental impulse. */
	void BeginMovement(float Distance, float Duration, const FVector& Impulse);

	void SpawnDodgeEffect(const UARPGElementalDodgeData& Data);

	/**
	 * Whether a dodge is legal from where the character is standing -- or not
	 * standing.
	 *
	 * Takes the readied element's dodge data rather than reading it back off the
	 * ability, because this is asked BEFORE the element is consumed: a refused
	 * dodge must not cost the player what they were holding.
	 */
	bool CanDodgeFromCurrentFooting(const class UARPGElementalDodgeData* Elemental) const;

	/** Direction from movement input, falling back to facing for a backstep. */
	FVector ResolveDodgeDirection(bool& bOutIsBackstep) const;

	UPROPERTY(Transient)
	TObjectPtr<UARPGMagicElement> DodgeElement;

	UPROPERTY(Transient)
	TObjectPtr<AActor> DodgeEffect;

	bool bGrantedInvincibility = false;
};
