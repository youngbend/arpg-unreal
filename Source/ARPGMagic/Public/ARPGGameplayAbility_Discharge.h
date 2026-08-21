// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "ARPGDischargeContext.h"
#include "ARPGMagicTypes.h"
#include "ARPGGameplayAbility_Discharge.generated.h"

class AARPGDischargeEffect;
class UAnimMontage;
class UARPGMagicComponent;

/**
 * Casts the readied element. Port of Godot's trigger_discharge plus the
 * charge/release loop animation.gd drove by hand.
 *
 * ONE CLASS, FOUR TYPES. Burst, emanate, project and cloak differ only in which
 * effect they spawn and which cost and damage knobs they read -- all of which
 * are keyed off DischargeType. The native subclasses below exist to carry
 * distinct ability tags so each can be blocked or cancelled independently, not
 * because their behaviour differs.
 *
 * COST IS NOT A COST GAMEPLAY EFFECT. See UARPGAbilityTask_ChargeDischarge: a
 * discharge drains continuously while held and fires at whatever fraction the
 * caster could afford, which a one-shot cost cannot express. CheckCost is still
 * overridden so activation is gated on affording the MINIMUM, matching Godot's
 * begin_discharge_charge returning false.
 */
UCLASS(Abstract)
class ARPGMAGIC_API UARPGGameplayAbility_Discharge : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UARPGGameplayAbility_Discharge();

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual bool CheckCost(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;

	virtual void InputReleased(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo) override;

	/** Which slot's costs, damage multipliers and effect this cast uses. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Discharge")
	EARPGDischargeType DischargeType = EARPGDischargeType::Burst;

	/** Seconds to a full charge. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Discharge",
		meta = (ClampMin = "0.01"))
	float MaxChargeTime = 1.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Discharge")
	TObjectPtr<UAnimMontage> CastMontage;

	/** How far in front of the caster the effect appears. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Discharge")
	float SpawnForwardOffset = 80.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Discharge")
	float SpawnHeightOffset = 60.f;

	/**
	 * Fire where the player is LOOKING rather than where the character happens to
	 * be facing. The character orients to its movement, so actor-forward throws a
	 * bolt sideways whenever the cast comes out mid-strafe -- which is most of
	 * them. Off by default: only the types that TRAVEL care where they were
	 * aimed, and an emanation or a cloak has no direction to get wrong.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Discharge")
	bool bAimAlongView = false;

	/**
	 * Degrees to tilt the aim UP off the view. The follow camera sits above and
	 * behind the caster looking slightly down, so a shot along the view alone
	 * buries itself in the ground a few metres out.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ARPG|Discharge",
		meta = (ClampMin = "-89.0", ClampMax = "89.0", EditCondition = "bAimAlongView"))
	float AimPitchOffset = 0.f;

	/**
	 * The view rotation a discharge actually fires along. Separate from
	 * GetAimDirection so the tilt is testable without a controller: the rotation
	 * a pawn reports arrives unnormalised (a pitch of -20 comes back as 340), and
	 * adding to that raw is how a nudge upward becomes a shot at the sky.
	 */
	static FVector ApplyAimPitch(const FRotator& ViewRotation, float PitchOffset);

protected:
	UFUNCTION()
	void OnChargeReleased(float ChargeFraction, bool bForced);

	UFUNCTION()
	void OnChargeFailed(float ChargeFraction, bool bForced);

	/**
	 * Spawns the effect and hands the cast to the magic component. Overridden by
	 * the cloak, which coats the caster rather than putting something in front of
	 * them.
	 */
	virtual void PerformDischarge(const FARPGDischargeContext& Context);

	UARPGMagicComponent* ResolveMagic() const;

	/** Where the effect appears and which way it points. */
	void GetSpawnTransform(FVector& OutOrigin, FVector& OutDirection) const;

	/** Which way this caster is pointing the cast. */
	FVector GetAimDirection(const AActor& Avatar) const;

	UPROPERTY(Transient)
	TObjectPtr<class UARPGAbilityTask_ChargeDischarge> ChargeTask;
};

UCLASS()
class ARPGMAGIC_API UARPGGameplayAbility_DischargeBurst : public UARPGGameplayAbility_Discharge
{
	GENERATED_BODY()

public:
	UARPGGameplayAbility_DischargeBurst();
};

UCLASS()
class ARPGMAGIC_API UARPGGameplayAbility_DischargeEmanate : public UARPGGameplayAbility_Discharge
{
	GENERATED_BODY()

public:
	UARPGGameplayAbility_DischargeEmanate();
};

UCLASS()
class ARPGMAGIC_API UARPGGameplayAbility_DischargeProject : public UARPGGameplayAbility_Discharge
{
	GENERATED_BODY()

public:
	UARPGGameplayAbility_DischargeProject();
};

/**
 * Coats the caster rather than throwing anything. Spawns its effect attached to
 * the caster and hands the duration to the cloak component.
 */
UCLASS()
class ARPGMAGIC_API UARPGGameplayAbility_DischargeCloak : public UARPGGameplayAbility_Discharge
{
	GENERATED_BODY()

public:
	UARPGGameplayAbility_DischargeCloak();

protected:
	virtual void PerformDischarge(const FARPGDischargeContext& Context) override;
};
