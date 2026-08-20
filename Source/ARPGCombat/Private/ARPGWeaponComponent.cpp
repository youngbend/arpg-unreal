// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGWeaponComponent.h"
#include "ARPGCombat.h"
#include "ARPGComboComponent.h"
#include "ARPGDamageTypeAsset.h"
#include "ARPGEquipmentEffect.h"
#include "ARPGGameplayTags.h"
#include "ARPGHitboxComponent.h"
#include "ARPGOffenseSet.h"
#include "ARPGParryComponent.h"
#include "ARPGWeaponAttackTree.h"
#include "ARPGWeaponDefinition.h"
#include "AbilitySystemComponent.h"
#include "Net/UnrealNetwork.h"

UARPGWeaponComponent::UARPGWeaponComponent()
{
	// Ticks only while the weapon is actually out; see SetDrawn.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	SetIsReplicatedByDefault(true); // clients need to know what is equipped
}

void UARPGWeaponComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	TickAutoSheathe(DeltaTime);
}

void UARPGWeaponComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UARPGWeaponComponent, Weapon);
	DOREPLIFETIME(UARPGWeaponComponent, bDrawn);
}

void UARPGWeaponComponent::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority() && DefaultWeapon && !Weapon)
	{
		EquipWeapon(DefaultWeapon);
		return;
	}

	// Runs with NOTHING equipped too, which is the point. The hitbox ships with
	// an authored WeaponBaseDamage, so a character who has never equipped
	// anything used to swing bare hands for a weapon's worth of damage -- and the
	// combo component's AttackTree was likewise left at whatever a Blueprint had
	// put there. Pushing the empty state makes unarmed mean unarmed from the
	// first frame.
	//
	// It is also the already-equipped path: a client whose Weapon replicated in
	// before this component began play.
	ApplyWeaponToOwner();
}

UARPGComboComponent* UARPGWeaponComponent::GetCombo() const
{
	return GetOwner() ? GetOwner()->FindComponentByClass<UARPGComboComponent>() : nullptr;
}

UARPGHitboxComponent* UARPGWeaponComponent::GetWeaponHitbox() const
{
	// Shared with the melee ability's own lookup, which used to be a second copy
	// of this loop with a different fallback rule.
	return UARPGHitboxComponent::FindOnActor(GetOwner(), EARPGHitboxSource::Weapon,
		/*bAllowFallback=*/false);
}

void UARPGWeaponComponent::EquipWeapon(UARPGWeaponDefinition* NewWeapon)
{
	if (Weapon == NewWeapon)
	{
		return;
	}

	Weapon = NewWeapon;
	ApplyWeaponToOwner();
	RefreshCritChance();
	OnWeaponChanged.Broadcast(Weapon);

	UE_LOG(LogARPGCombat, Log, TEXT("%s equipped '%s' (tree %s)"),
		*GetNameSafe(GetOwner()),
		Weapon ? *Weapon->WeaponId.ToString() : TEXT("<none>"),
		Weapon && Weapon->AttackTree ? *Weapon->AttackTree->GetName() : TEXT("<none>"));
}

void UARPGWeaponComponent::UnequipWeapon()
{
	EquipWeapon(nullptr);

	// Sheathed state is meaningless with nothing equipped, and leaving it true
	// would have the animation layer holding a drawn stance over empty hands.
	SetDrawn(false);
}

void UARPGWeaponComponent::ApplyWeaponToOwner()
{
	// The moveset comes from the weapon. Cleared on unequip so an unarmed
	// character cannot keep swinging the sword's combo tree.
	if (UARPGComboComponent* Combo = GetCombo())
	{
		Combo->AttackTree = Weapon ? Weapon->AttackTree : nullptr;
		Combo->ResetCombo();

		if (Weapon && !Weapon->AttackTree)
		{
			UE_LOG(LogARPGCombat, Warning,
				TEXT("Weapon '%s' has no attack tree; melee attacks will do nothing."),
				*Weapon->WeaponId.ToString());
		}
	}

	// WeaponBaseDamage, not BaseDamage: the ability rewrites BaseDamage per swing
	// as weapon damage x motion value, so writing there would be overwritten and
	// writing it repeatedly would compound.
	if (UARPGHitboxComponent* Hitbox = GetWeaponHitbox())
	{
		Hitbox->WeaponBaseDamage = Weapon ? Weapon->BaseDamage : UnarmedBaseDamage;
		if (Weapon && Weapon->BaseDamageType)
		{
			Hitbox->DamageType = Weapon->BaseDamageType;
		}
	}
}

void UARPGWeaponComponent::RefreshCritChance()
{
	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC || !HasAuthority())
	{
		return;
	}

	const float Bonus = Weapon ? Weapon->GetTotalCritChanceBonus() : 0.f;

	TArray<TPair<FGameplayAttribute, float>> Grants;
	if (Bonus != 0.f)
	{
		Grants.Emplace(UARPGOffenseSet::GetCritChanceAttribute(), Bonus);
	}

	// No clamp to [0,1] here any more: the hitbox already clamps the TOTAL roll
	// when it makes one, and clamping the contribution would silently discard
	// part of a bonus that a later debuff would have made room for.
	UARPGEquipmentEffectLibrary::ApplyGrant(ASC, Grants, CritGrantHandle);
}

void UARPGWeaponComponent::SetDrawn(bool bNewDrawn)
{
	if (bDrawn == bNewDrawn)
	{
		return;
	}

	bDrawn = bNewDrawn;
	OnWeaponDrawnChanged.Broadcast(bDrawn);

	// Drawing IS combat activity, so the countdown starts from the draw rather
	// than from whatever happened before it.
	TimeSinceCombatActivity = 0.f;
	SetComponentTickEnabled(bDrawn);

	// Sheathing abandons the combo: redrawing mid-reset-timer would otherwise
	// resume the chain from wherever it was left off, which reads as the
	// character remembering a swing they put away.
	if (!bDrawn)
	{
		if (UARPGComboComponent* Combo = GetCombo())
		{
			Combo->ResetCombo();
		}
	}
}

// ---------------------------------------------------------------------------
// Auto-sheathe
// ---------------------------------------------------------------------------

void UARPGWeaponComponent::NotifyCombatActivity()
{
	TimeSinceCombatActivity = 0.f;
}

void UARPGWeaponComponent::TickAutoSheathe(float DeltaTime)
{
	const AActor* Owner = GetOwner();
	if (!bAutoSheatheEnabled || !bDrawn || !Owner || !Owner->HasAuthority())
	{
		return;
	}

	// Abandoned entirely while dead, timer and all. The death animation and the
	// respawn both put the weapon away on their own terms, and a sheathe that
	// fired during the death window would resolve visually at the far end of it
	// -- the character appears to put their sword away the instant they spawn.
	const UAbilitySystemComponent* ASC = GetASC();
	if (ASC && ASC->HasMatchingGameplayTag(TAG_State_Dead))
	{
		TimeSinceCombatActivity = 0.f;
		return;
	}

	// Mid-swing or mid-block is activity by definition, and neither reports
	// itself frame by frame -- a long channel would otherwise time out and
	// sheathe the weapon it is still swinging.
	const UARPGComboComponent* Combo = GetCombo();
	const UARPGParryComponent* Parry = Owner->FindComponentByClass<UARPGParryComponent>();
	if ((Combo && Combo->IsAttacking()) || (Parry && Parry->IsBlocking()))
	{
		TimeSinceCombatActivity = 0.f;
		return;
	}

	TimeSinceCombatActivity += DeltaTime;
	if (TimeSinceCombatActivity < AutoSheatheDelay)
	{
		return;
	}

	if (bAutoSheatheSuppressed)
	{
		// HELD at the threshold, not reset -- see SetAutoSheatheSuppressed. The
		// weapon goes away on the first frame after the last enemy loses
		// interest, not a full delay later.
		TimeSinceCombatActivity = AutoSheatheDelay;
		return;
	}

	SetDrawn(false);
}

float UARPGWeaponComponent::GetEffectiveDamage(float MotionValue) const
{
	if (!Weapon)
	{
		return 0.f;
	}

	float Damage = Weapon->GetEffectiveDamage(MotionValue);

	// The same outgoing multiplier magic discharges use, so one Weakened status
	// weakens both rather than needing a second, parallel debuff for weapons.
	if (const UAbilitySystemComponent* ASC = GetASC())
	{
		const float Amp = ASC->GetNumericAttribute(
			UARPGOffenseSet::GetDamageAmpMultiplierAttribute());
		if (Amp > 0.f)
		{
			Damage *= Amp;
		}
	}

	return Damage;
}

void UARPGWeaponComponent::OnRep_Weapon()
{
	ApplyWeaponToOwner();
	OnWeaponChanged.Broadcast(Weapon);
}

void UARPGWeaponComponent::OnRep_Drawn()
{
	OnWeaponDrawnChanged.Broadcast(bDrawn);
}
