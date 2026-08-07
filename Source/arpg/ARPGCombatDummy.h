// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AbilitySystemInterface.h"
#include "GameplayTagContainer.h"
#include "ARPGCombatDummy.generated.h"

class UCapsuleComponent;
class UStaticMeshComponent;
class UARPGAbilitySystemComponent;
class UARPGHurtboxComponent;
class UARPGVitalSet;
class UARPGOffenseSet;
class UARPGResistanceSet;
class UARPGDamageTypeAsset;
struct FOnAttributeChangeData;

/**
 * A thing you can hit, so the damage pipeline is observable before there is any
 * combat UI, weapon, or animation to observe it through.
 *
 * Owns its ASC directly rather than via a PlayerState -- the NPC arrangement,
 * and the reason its replication mode is Minimal: nobody owns this, so no client
 * needs full gameplay-effect detail for it.
 *
 * Health changes are logged with the damage number, which is the Phase 1 gate:
 * hit it and check the number matches what armor and resistance should have
 * produced.
 */
UCLASS()
class AARPGCombatDummy : public AActor, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AARPGCombatDummy();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

	// Public so automation tests can configure a dummy between SpawnActorDeferred
	// and FinishSpawning, which is the only window before BeginPlay reads them.

	/** Which side this dummy is on. Enemy by default, so player hits land. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|Dummy",
		meta = (Categories = "Faction"))
	FGameplayTag FactionTag;

	/** Starting/maximum health. Applied on BeginPlay. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|Dummy",
		meta = (ClampMin = "1.0"))
	float MaxHealth = 100.f;

	/** Flat physical armor, subtracted before resistance. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|Dummy")
	float BaseArmor = 0.f;

	/**
	 * Per-type resistances. 0.5 = half damage, 1.0 = immune, -0.5 = takes 1.5x.
	 *
	 * Keyed by the damage type ASSET rather than its tag, deliberately: the asset
	 * already names which attribute answers for it, so this needs no second copy
	 * of the tag-to-attribute mapping that would then have to be kept in sync.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|Dummy")
	TMap<TObjectPtr<UARPGDamageTypeAsset>, float> Resistances;

	/** Logs every health change with the delta. The Phase 1 gate reads this. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|Dummy")
	bool bLogHealthChanges = true;

protected:
	virtual void BeginPlay() override;

private:
	void OnHealthChanged(const FOnAttributeChangeData& Data);

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UCapsuleComponent> Capsule;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UARPGHurtboxComponent> Hurtbox;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UARPGAbilitySystemComponent> AbilitySystemComponent;

	UPROPERTY()
	TObjectPtr<UARPGVitalSet> VitalSet;

	UPROPERTY()
	TObjectPtr<UARPGOffenseSet> OffenseSet;

	UPROPERTY()
	TObjectPtr<UARPGResistanceSet> ResistanceSet;
};
