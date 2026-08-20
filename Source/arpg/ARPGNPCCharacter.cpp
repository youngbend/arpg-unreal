// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGNPCCharacter.h"
#include "ARPGAIController.h"
#include "ARPGAbilitySystemComponent.h"
#include "ARPGComboComponent.h"
#include "ARPGHitboxComponent.h"
#include "ARPGHurtboxComponent.h"
#include "ARPGInventoryComponent.h"
#include "ARPGNPCComponent.h"
#include "ARPGHitStopComponent.h"
#include "ARPGNoiseComponent.h"
#include "ARPGStatusResistanceComponent.h"
#include "ARPGOffenseSet.h"
#include "ARPGParryComponent.h"
#include "ARPGPoiseComponent.h"
#include "ARPGQuickSlotComponent.h"
#include "ARPGResistanceSet.h"
#include "ARPGVitalSet.h"
#include "ARPGWeaponComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

AARPGNPCCharacter::AARPGNPCCharacter()
{
	PrimaryActorTick.bCanEverTick = false;

	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.f);

	// Possessed by OUR controller, which is what brings perception and the
	// StateTree with it. Without this an NPC placed in a level is a body with
	// nobody driving it -- which is a legitimate state, but not the default one.
	AIControllerClass = AARPGAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;

	// The controller overwrites all three from the archetype on possession. These
	// are only what an unpossessed or definition-less NPC falls back to.
	bUseControllerRotationYaw = false;
	GetCharacterMovement()->bOrientRotationToMovement = true;
	GetCharacterMovement()->RotationRate = FRotator(0.f, 360.f, 0.f);
	GetCharacterMovement()->MaxWalkSpeed = 400.f;

	// Minimal, not Mixed: nothing owns an NPC, so no client needs the full
	// gameplay-effect list for it. Port plan §3.1.
	Combatant.Create(*this, EGameplayEffectReplicationMode::Minimal);

	Hurtbox = CreateDefaultSubobject<UARPGHurtboxComponent>(TEXT("Hurtbox"));

	// Riding the hand bone, because the parry prediction reads this component's
	// live world position -- see the header. The socket name is the UE5
	// mannequin's; a skeleton without it leaves the hitbox on the mesh origin,
	// which the parry will happily measure and get wrong.
	WeaponHitbox = CreateDefaultSubobject<UARPGHitboxComponent>(TEXT("WeaponHitbox"));
	WeaponHitbox->SetupAttachment(GetMesh(), TEXT("hand_r"));
	WeaponHitbox->HitboxSource = EARPGHitboxSource::Weapon;
	WeaponHitbox->TraceRadius = 40.f;
	WeaponHitbox->DeactivateHitbox();

	ComboComponent  = CreateDefaultSubobject<UARPGComboComponent>(TEXT("ComboComponent"));
	WeaponComponent = CreateDefaultSubobject<UARPGWeaponComponent>(TEXT("WeaponComponent"));
	ParryComponent  = CreateDefaultSubobject<UARPGParryComponent>(TEXT("ParryComponent"));
	PoiseComponent  = CreateDefaultSubobject<UARPGPoiseComponent>(TEXT("PoiseComponent"));

	InventoryComponent = CreateDefaultSubobject<UARPGInventoryComponent>(TEXT("InventoryComponent"));
	QuickSlotComponent = CreateDefaultSubobject<UARPGQuickSlotComponent>(TEXT("QuickSlotComponent"));

	NoiseComponent = CreateDefaultSubobject<UARPGNoiseComponent>(TEXT("NoiseComponent"));

	// See the header: hit-stop needs a component at BOTH ends of an exchange,
	// and status resistance was doing nothing on the actor it is aimed at.
	HitStopComponent = CreateDefaultSubobject<UARPGHitStopComponent>(TEXT("HitStopComponent"));
	StatusResistance = CreateDefaultSubobject<UARPGStatusResistanceComponent>(TEXT("StatusResistance"));

	NPCComponent = CreateDefaultSubobject<UARPGNPCComponent>(TEXT("NPCComponent"));
}

UAbilitySystemComponent* AARPGNPCCharacter::GetAbilitySystemComponent() const
{
	return Combatant.AbilitySystem;
}

void AARPGNPCCharacter::BeginPlay()
{
	// BEFORE Super, and the ordering is load-bearing. AActor::BeginPlay dispatches
	// BeginPlay to every component, and UARPGNPCComponent's writes the archetype's
	// health and poise through this ASC. Initialising afterwards would mean those
	// writes land on an ability system with no actor info -- which mostly works,
	// right up until an attribute-set callback reaches for an avatar that is not
	// there yet.
	if (Combatant.AbilitySystem)
	{
		Combatant.AbilitySystem->InitAbilityActorInfo(this, this);
	}

	Super::BeginPlay();
}
