// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ARPGDischargeContext.h"
#include "ARPGCloakComponent.generated.h"

class AActor;
class UARPGMagicElement;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnCloakChanged, UARPGMagicElement*, Element);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnShieldAbsorbed, float, Absorbed);

/**
 * The caster's elemental cloak. Port of Godot's CloakComponent.
 *
 * WHY CLOAK IS A COMPONENT WHEN THE OTHER THREE DISCHARGES ARE JUST ABILITIES.
 * Burst, emanate and project are events: they spawn something and are done. A
 * cloak is persistent state that other systems have to be able to ask about --
 * incoming damage checks the shield, the damage pipeline may reflect off it, VFX
 * has to know it is still up. State that outlives its ability wants an owner
 * that outlives the ability too.
 *
 * SHIELD REUSES ComputedDamage AS CAPACITY for "defensive"-tagged elements.
 * Deliberately the same number rather than a second authored field: a defensive
 * element's power should scale with charge and mastery exactly as an offensive
 * one's does, and giving it a parallel field would mean tuning both.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGMAGIC_API UARPGCloakComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGCloakComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Used when the element sets no CloakBaseDuration of its own. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Cloak",
		meta = (ClampMin = "0.0"))
	float DefaultDuration = 10.f;

	/**
	 * An element tagged with this treats its cloak as a shield.
	 *
	 * A tag rather than a bool on the element so the same classification can be
	 * read by anything else that cares -- the Godot version tested a "defensive"
	 * string in the element's tag array for exactly this.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Cloak")
	FGameplayTag DefensiveElementTag;

	/**
	 * How often the element's self-effect is re-applied while cloaked.
	 *
	 * The self-effect is otherwise one application at cast time, which can fade
	 * on its own duration well before a long cloak expires -- leaving a fire
	 * cloak that visibly burns but stops hurting. 0 applies it once only.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Cloak",
		meta = (ClampMin = "0.0"))
	float SelfEffectInterval = 0.f;

	/** Server-side. Called by the cloak discharge ability. */
	void ApplyCloak(const FARPGDischargeContext& Context);

	/** Cancel from outside -- a dispel. No-op when not cloaked. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Cloak")
	void BreakCloak();

	UFUNCTION(BlueprintPure, Category = "ARPG|Cloak")
	bool IsCloaked() const { return ActiveElement != nullptr; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Cloak")
	UARPGMagicElement* GetCloakElement() const { return ActiveElement; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Cloak")
	float GetCloakRemaining() const { return CloakTimer; }

	UFUNCTION(BlueprintPure, Category = "ARPG|Cloak")
	float GetShieldRemaining() const { return ShieldRemaining; }

	/**
	 * Depletes the shield by up to Incoming and returns what it absorbed.
	 * Called by the damage execution before health is reduced.
	 */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Cloak")
	float AbsorbDamage(float Incoming);

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Cloak")
	FARPGOnCloakChanged OnCloakApplied;

	/** Fires on both expiry and break -- the element is null either way. */
	UPROPERTY(BlueprintAssignable, Category = "ARPG|Cloak")
	FARPGOnCloakChanged OnCloakEnded;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Cloak")
	FARPGOnShieldAbsorbed OnShieldAbsorbed;

protected:
	UFUNCTION()
	void OnRep_ActiveElement();

private:
	void EndCloak();
	void ApplySelfEffect();

	UPROPERTY(ReplicatedUsing = OnRep_ActiveElement)
	TObjectPtr<UARPGMagicElement> ActiveElement;

	UPROPERTY(Replicated)
	float ShieldRemaining = 0.f;

	UPROPERTY(Transient)
	TObjectPtr<AActor> CloakEffect;

	float CloakTimer = 0.f;
	float SelfEffectTimer = 0.f;
};
