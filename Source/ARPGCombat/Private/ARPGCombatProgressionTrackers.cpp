// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGCombatProgressionTrackers.h"
#include "ARPGArmorComponent.h"
#include "ARPGArmorDefinition.h"
#include "ARPGCombat.h"
#include "ARPGHitboxComponent.h"
#include "ARPGHurtboxComponent.h"
#include "ARPGWeaponComponent.h"
#include "ARPGWeaponDefinition.h"
#include "GameFramework/Actor.h"

// ---------------------------------------------------------------------------
// Weapon
// ---------------------------------------------------------------------------

void UARPGWeaponProgressionTracker::BindXPSource()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// Every hitbox, not just the weapon's: an unarmed kick progresses
	// Weapon.Unarmed, which is what a player expects from a game that tracks
	// what you actually fight with.
	TArray<UARPGHitboxComponent*> Hitboxes;
	Owner->GetComponents<UARPGHitboxComponent>(Hitboxes);

	for (UARPGHitboxComponent* Hitbox : Hitboxes)
	{
		Hitbox->OnHitLanded.AddDynamic(this, &UARPGWeaponProgressionTracker::HandleHitLanded);
	}

	if (Hitboxes.Num() == 0)
	{
		UE_LOG(LogARPGCombat, Warning,
			TEXT("Weapon progression tracker on %s found no hitbox, so it can never earn XP."),
			*GetNameSafe(Owner));
	}
}

void UARPGWeaponProgressionTracker::HandleHitLanded(AActor* HitActor, const FHitResult& Hit, float Damage)
{
	const UARPGWeaponComponent* Weapon =
		GetOwner() ? GetOwner()->FindComponentByClass<UARPGWeaponComponent>() : nullptr;

	const UARPGWeaponDefinition* Definition = Weapon ? Weapon->GetWeapon() : nullptr;

	// No weapon component, or nothing equipped, is unarmed rather than nothing:
	// the punch still happened and still teaches the player something.
	const FGameplayTag TypeTag = UARPGWeaponDefinition::WeaponTypeToTag(
		Definition ? Definition->WeaponType : EARPGWeaponType::Unarmed);

	RecordUse(TypeTag, Damage * XPPerDamage);
}

// ---------------------------------------------------------------------------
// Armour
// ---------------------------------------------------------------------------

void UARPGArmorProgressionTracker::BindXPSource()
{
	AActor* Owner = GetOwner();
	UARPGHurtboxComponent* Hurtbox =
		Owner ? Owner->FindComponentByClass<UARPGHurtboxComponent>() : nullptr;

	if (!Hurtbox)
	{
		UE_LOG(LogARPGCombat, Warning,
			TEXT("Armour progression tracker on %s found no hurtbox, so it can never earn XP."),
			*GetNameSafe(Owner));
		return;
	}

	Hurtbox->OnHitReceived.AddDynamic(this, &UARPGArmorProgressionTracker::HandleHitReceived);
}

void UARPGArmorProgressionTracker::HandleHitReceived(const FGameplayEffectContextHandle& Context,
	float Magnitude)
{
	const UARPGArmorComponent* Armor =
		GetOwner() ? GetOwner()->FindComponentByClass<UARPGArmorComponent>() : nullptr;

	const UARPGArmorDefinition* Definition = Armor ? Armor->GetArmor() : nullptr;

	// Unarmoured is a type you can get better at wearing -- dodging in cloth is
	// a skill, and excluding it would leave the lightest build unable to
	// progress its defensive category at all.
	const FGameplayTag TypeTag = UARPGArmorDefinition::ArmorTypeToTag(
		Definition ? Definition->ArmorType : EARPGArmorType::Unarmored);

	RecordUse(TypeTag, Magnitude * XPPerDamage);
}
