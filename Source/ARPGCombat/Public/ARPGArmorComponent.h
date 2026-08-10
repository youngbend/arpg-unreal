// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ARPGGameplayComponentBase.h"
#include "GameplayTagContainer.h"
#include "AttributeSet.h"
#include "GameplayEffectTypes.h"
#include "ARPGArmorComponent.generated.h"

class UARPGArmorDefinition;
class UAbilitySystemComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FARPGOnArmorChanged, UARPGArmorDefinition*, Armor);

/**
 * Holds the equipped armour. Port of Godot's ArmorComponent.
 *
 * WHY THIS GRANTS ATTRIBUTES RATHER THAN BEING CONSULTED AT HIT TIME. Godot's
 * receive_damage() reached into the ArmorComponent mid-pipeline to add its bonus
 * onto CombatStats.base_armor. Here armour instead contributes to the BaseArmor
 * and resistance attributes for as long as it is worn.
 *
 * That is the better shape in GAS for a reason worth stating: it means the
 * damage execution reads ONE number per stat, and everything that can change
 * that number -- archetype, armour, a Wet status, a timed buff -- composes
 * through the same aggregator instead of each adding a bespoke branch to the
 * execution. The execution stays a formula rather than a list of special cases.
 *
 * THE CONTRIBUTION IS A GAMEPLAY EFFECT, not a write to the attribute. This
 * component used to add its amount onto the base value and remember it so it
 * could subtract the same number later, which was wrong twice over -- see
 * UARPGEquipmentGameplayEffect. As a modifier the aggregator owns it, removal is
 * exact by construction, and there is no bookkeeping to keep in step.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGCOMBAT_API UARPGArmorComponent : public UARPGGameplayComponentBase
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
	/** Removes the previous grant and applies the current piece's. */
	void RefreshAttributes();

	UPROPERTY(ReplicatedUsing = OnRep_Armor)
	TObjectPtr<UARPGArmorDefinition> Armor;

	/**
	 * The live grant, or invalid when nothing is worn.
	 *
	 * One handle replaces the previous AppliedArmorValue plus a parallel array of
	 * applied resistances: removing the effect reverses every modifier it carried
	 * at once, exactly, whatever else has happened to those attributes since.
	 */
	FActiveGameplayEffectHandle GrantHandle;
};
