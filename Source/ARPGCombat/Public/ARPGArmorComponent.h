// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "AttributeSet.h"
#include "ARPGArmorComponent.generated.h"

class UARPGArmorDefinition;
class UAbilitySystemComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnArmorChanged, UARPGArmorDefinition*, Armor);

/**
 * Holds the equipped armour. Port of Godot's ArmorComponent.
 *
 * WHY THIS WRITES ATTRIBUTES RATHER THAN BEING CONSULTED AT HIT TIME. Godot's
 * receive_damage() reached into the ArmorComponent mid-pipeline to add its bonus
 * onto CombatStats.base_armor. Here armour instead applies its contribution to
 * the BaseArmor and resistance attributes on equip, and removes it on unequip.
 *
 * That is the better shape in GAS for a reason worth stating: it means the
 * damage execution reads ONE number per stat, and everything that can change
 * that number -- archetype, armour, a Wet status, a timed buff -- composes
 * through the same aggregator instead of each adding a bespoke branch to the
 * execution. The execution stays a formula rather than a list of special cases.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCOMBAT_API UARPGArmorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGArmorComponent();

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG|Armor")
	TObjectPtr<UARPGArmorDefinition> DefaultArmor;

	UFUNCTION(BlueprintCallable, Category = "ARPG|Armor")
	void EquipArmor(UARPGArmorDefinition* NewArmor);

	UFUNCTION(BlueprintCallable, Category = "ARPG|Armor")
	void UnequipArmor();

	UFUNCTION(BlueprintPure, Category = "ARPG|Armor")
	UARPGArmorDefinition* GetArmor() const { return Armor; }

	/** Progression key for the equipped type, e.g. "heavy". */
	UFUNCTION(BlueprintPure, Category = "ARPG|Armor")
	FName GetArmorTypeName() const;

	UPROPERTY(BlueprintAssignable, Category = "ARPG|Armor")
	FARPGOnArmorChanged OnArmorChanged;

protected:
	UFUNCTION()
	void OnRep_Armor();

private:
	/** Removes exactly what was previously applied, then applies the current piece. */
	void RefreshAttributes();

	UAbilitySystemComponent* GetASC() const;

	UPROPERTY(ReplicatedUsing = OnRep_Armor)
	TObjectPtr<UARPGArmorDefinition> Armor;

	/**
	 * What this component last added, so unequipping subtracts exactly that.
	 *
	 * Recomputing the contribution from the definition at removal time would be
	 * wrong the moment a modifier is rerolled or the definition is edited while
	 * worn -- the amount actually applied is the only safe thing to reverse.
	 */
	float AppliedArmorValue = 0.f;

	/** Attribute plus the amount this component added to it. */
	TArray<TPair<FGameplayAttribute, float>> AppliedResistances;

	UPROPERTY(Transient)
	mutable TObjectPtr<UAbilitySystemComponent> CachedASC;
};
