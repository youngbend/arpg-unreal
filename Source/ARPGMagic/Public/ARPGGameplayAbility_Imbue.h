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
 * THE ELEMENTAL HITBOX IS SEPARATE FROM THE WEAPON'S. The weapon deals its own
 * physical damage and the imbue deals elemental damage on the same swing, as two
 * independent hits, from two hitboxes. Folding them into one would force a
 * single damage type through the mitigation pipeline and lose exactly the
 * interaction that makes imbuing worthwhile -- armour resists the steel,
 * resistance resists the fire, and they are not the same number. And a coating
 * reaches further than the edge it is on, which one hitbox cannot express at
 * all: see FARPGElementalCoating::TraceRadiusScale.
 *
 * The ability OWNS that hitbox, creating one at runtime when the character has
 * no authored EARPGHitboxSource::Elemental of its own, so imbuing works on every
 * character without anyone having to remember to add a component. It is paired
 * with the weapon's hitbox for the duration of each window, which is what stops
 * the two halves cancelling each other out on the target's i-frames -- see
 * UARPGHitboxComponent::PairWithSwingPartner.
 *
 * THE SWING DRIVES THIS, NOT THE OTHER WAY AROUND. The ability holds the coating
 * and waits; UARPGGameplayAbility_MeleeAttack raises it for each hitbox window,
 * takes it down again, and tells it when the attack ended, through
 * IARPGSwingAugment. Combat cannot name anything in ARPGMagic -- see that
 * interface for why it has to be this way round.
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
	 * Brings up the elemental hitbox alongside SwingHitbox for one window, scaled
	 * by that window's motion value. Returns false when there is nothing to arm.
	 *
	 * ACCOMPANIES, NEVER REWRITES. SwingHitbox is read for its transform, reach
	 * and trace settings and is otherwise left exactly as the attack armed it --
	 * its damage and damage type are the WEAPON's, and writing the element over
	 * them would replace the physical hit instead of adding to it.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Imbue")
	bool ArmElementalHitbox(class UARPGHitboxComponent* SwingHitbox, float MotionValue);

	/** Takes the elemental hitbox back down. The coating itself survives. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Imbue")
	void DisarmElementalHitbox();

	/** Spends the imbue and ends the ability, dropping the coating and its VFX. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Imbue")
	void ConsumeImbue();

	// --- IARPGSwingAugment ----------------------------------------------------

	virtual void ArmSwingAugment_Implementation(UARPGHitboxComponent* SwingHitbox,
		float MotionValue) override;
	virtual void DisarmSwingAugment_Implementation() override;
	virtual void NotifySwingEnded_Implementation() override;

private:
	void SpawnImbueEffect();

	/**
	 * The character's authored elemental hitbox, or a fresh one attached to the
	 * swing's hitbox so it traces the same arc at its own size.
	 *
	 * Attached to the WEAPON HITBOX rather than to a socket, in the created case:
	 * a coating follows the blade, and whatever the attack chose to swing --
	 * blade or boot -- is exactly what this needs to follow, at whatever offset
	 * that hitbox has been authored with.
	 */
	UARPGHitboxComponent* ResolveElementalHitbox(UARPGHitboxComponent* SwingHitbox);

	/** Destroys the elemental hitbox if this ability was the one that made it. */
	void ReleaseElementalHitbox();

	UPROPERTY(Transient)
	TObjectPtr<UARPGMagicElement> ImbuedElement;

	UPROPERTY(Transient)
	TObjectPtr<AActor> ImbueEffect;

	UPROPERTY(Transient)
	TObjectPtr<UARPGHitboxComponent> ElementalHitbox;

	/**
	 * Whether ElementalHitbox is ours to destroy. False when the character
	 * authored one -- borrowing a component does not entitle us to delete it.
	 */
	bool bOwnsElementalHitbox = false;

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
