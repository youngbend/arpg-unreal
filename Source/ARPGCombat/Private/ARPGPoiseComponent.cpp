// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGPoiseComponent.h"
#include "ARPGReactionDefinitions.h"
#include "ARPGWeaponAttackTree.h"
#include "ARPGWeaponComponent.h"
#include "ARPGWeaponDefinition.h"
#include "Animation/AnimInstance.h"
#include "GameFramework/Character.h"
#include "ARPGGameplayTags.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "GameplayEffectTypes.h"

UARPGPoiseComponent::UARPGPoiseComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	SetIsReplicatedByDefault(false); // resolved server-side; the Poise attribute replicates
}

void UARPGPoiseComponent::BeginPlay()
{
	Super::BeginPlay();
	EnsureSubscribed();
}

void UARPGPoiseComponent::EnsureSubscribed()
{
	if (PoiseDamageHandle.IsValid())
	{
		return;
	}

	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC)
	{
		// Not an error: a player's ASC arrives with the PlayerState, which may
		// be later than this component's BeginPlay. Tick retries.
		return;
	}

	// Subscribing to the attribute set rather than polling the attribute: the
	// SIZE of each individual contribution is what decides a flinch tier, and
	// that information exists only at the moment it arrives.
	if (const UARPGVitalSet* VitalSet =
			Cast<UARPGVitalSet>(ASC->GetAttributeSet(UARPGVitalSet::StaticClass())))
	{
		PoiseDamageHandle = const_cast<UARPGVitalSet*>(VitalSet)->OnPoiseDamageReceived
			.AddUObject(this, &UARPGPoiseComponent::HandlePoiseDamageReceived);
	}
}

void UARPGPoiseComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (PoiseDamageHandle.IsValid())
	{
		if (UAbilitySystemComponent* ASC = GetASC())
		{
			if (const UARPGVitalSet* VitalSet =
					Cast<UARPGVitalSet>(ASC->GetAttributeSet(UARPGVitalSet::StaticClass())))
			{
				const_cast<UARPGVitalSet*>(VitalSet)->OnPoiseDamageReceived.Remove(PoiseDamageHandle);
			}
		}
		PoiseDamageHandle.Reset();
	}

	Super::EndPlay(EndPlayReason);
}

UAbilitySystemComponent* UARPGPoiseComponent::GetASC() const
{
	if (!CachedASC)
	{
		CachedASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetOwner());
	}
	return CachedASC;
}

float UARPGPoiseComponent::GetCurrentPoise() const
{
	const UAbilitySystemComponent* ASC = GetASC();
	return ASC ? ASC->GetNumericAttribute(UARPGVitalSet::GetPoiseAttribute()) : 0.f;
}

float UARPGPoiseComponent::GetMaxPoise() const
{
	const UAbilitySystemComponent* ASC = GetASC();
	const float Max = ASC ? ASC->GetNumericAttribute(UARPGVitalSet::GetMaxPoiseAttribute()) : 0.f;
	return Max > 0.f ? Max : 100.f;
}

void UARPGPoiseComponent::SetPoise(float NewValue)
{
	if (UAbilitySystemComponent* ASC = GetASC())
	{
		ASC->SetNumericAttributeBase(UARPGVitalSet::GetPoiseAttribute(),
			FMath::Clamp(NewValue, 0.f, GetMaxPoise()));
		OnPoiseChanged.Broadcast(GetCurrentPoise(), GetMaxPoise());
	}
}

void UARPGPoiseComponent::HandlePoiseDamageReceived(float Amount)
{
	ApplyPoiseDamage(Amount);
}

EARPGPoiseResult UARPGPoiseComponent::ApplyPoiseDamage(float Amount)
{
	if (Amount <= 0.f)
	{
		return EARPGPoiseResult::None;
	}

	// Fully immune to ACCUMULATION during the post-break window, not merely to
	// breaking again. Letting the meter refill during recovery would mean a
	// second break landing the instant immunity lapsed, which is the chain-break
	// this window exists to prevent.
	if (BreakImmunityTimer > 0.f)
	{
		return EARPGPoiseResult::None;
	}

	const float MaxPoise = GetMaxPoise();

	SetPoise(GetCurrentPoise() + Amount);
	HoldTimer = HoldTime;

	// Stance break, checked BEFORE the hyperarmor gate below. A hyperarmored
	// attacker shrugs off stagger but can still have their stance broken --
	// otherwise hyperarmor would be an unanswerable defence rather than a
	// trade-off.
	if (GetCurrentPoise() >= MaxPoise)
	{
		SetPoise(0.f);
		BreakImmunityTimer = BreakImmunityTime;
		HoldTimer = 0.f;

		OnPoiseResult.Broadcast(EARPGPoiseResult::StanceBreak);
		SendPoiseEvent(EARPGPoiseResult::StanceBreak);
		PlayReactionMontage(EARPGPoiseResult::StanceBreak);
		return EARPGPoiseResult::StanceBreak;
	}

	// Hyperarmor replaces the Godot version's set_attack_hyperarmor() flag,
	// which animation.gd had to push every frame. Here it is just a tag the
	// attacking ability grants itself.
	const UAbilitySystemComponent* ASC = GetASC();
	if (ASC && ASC->HasMatchingGameplayTag(TAG_State_Hyperarmor))
	{
		return EARPGPoiseResult::None;
	}

	// Keyed on THIS contribution, not the running total, and neither tier
	// resets the meter. Heavy is tested first so a blow big enough for both
	// only fires heavy.
	if (Amount >= HeavyFlinchThresholdPct * MaxPoise)
	{
		OnPoiseResult.Broadcast(EARPGPoiseResult::HeavyFlinch);
		SendPoiseEvent(EARPGPoiseResult::HeavyFlinch);
		PlayReactionMontage(EARPGPoiseResult::HeavyFlinch);
		return EARPGPoiseResult::HeavyFlinch;
	}

	if (Amount >= LightFlinchThresholdPct * MaxPoise)
	{
		OnPoiseResult.Broadcast(EARPGPoiseResult::LightFlinch);
		SendPoiseEvent(EARPGPoiseResult::LightFlinch);
		PlayReactionMontage(EARPGPoiseResult::LightFlinch);
		return EARPGPoiseResult::LightFlinch;
	}

	return EARPGPoiseResult::None;
}

void UARPGPoiseComponent::ResetPoise()
{
	SetPoise(0.f);
	HoldTimer = 0.f;
}

void UARPGPoiseComponent::SendPoiseEvent(EARPGPoiseResult Result) const
{
	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC)
	{
		return;
	}

	FGameplayTag EventTag;
	switch (Result)
	{
	case EARPGPoiseResult::LightFlinch: EventTag = TAG_Event_Poise_LightFlinch; break;
	case EARPGPoiseResult::HeavyFlinch: EventTag = TAG_Event_Poise_HeavyFlinch; break;
	case EARPGPoiseResult::StanceBreak: EventTag = TAG_Event_Poise_StanceBreak; break;
	default: return;
	}

	FGameplayEventData EventData;
	EventData.EventTag = EventTag;
	EventData.Target = ASC->GetAvatarActor();

	// Routed as a gameplay event rather than a direct call so the reaction is an
	// ability: it can then be blocked by tags, cancel the attack in progress,
	// and play its own montage, none of which a callback could express.
	ASC->HandleGameplayEvent(EventTag, &EventData);
}

void UARPGPoiseComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Cheap no-op once bound; covers the ASC arriving after BeginPlay.
	EnsureSubscribed();

	if (BreakImmunityTimer > 0.f)
	{
		BreakImmunityTimer = FMath::Max(0.f, BreakImmunityTimer - DeltaTime);
	}

	const float Current = GetCurrentPoise();
	if (Current <= 0.f)
	{
		return;
	}

	// Hold, then drain -- not a constant bleed. The hold is what makes a flurry
	// of small hits able to reach a break at all; without it the meter would
	// drain between blows and the top would be unreachable.
	if (HoldTimer > 0.f)
	{
		HoldTimer = FMath::Max(0.f, HoldTimer - DeltaTime);
		return;
	}

	if (DrainRate > 0.f)
	{
		SetPoise(Current - DrainRate * DeltaTime);
	}
}

void UARPGPoiseComponent::PlayReactionMontage(EARPGPoiseResult Result) const
{
	if (!bPlayReactionMontages)
	{
		return;
	}

	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	if (!Character)
	{
		return;
	}

	const UARPGWeaponComponent* Weapon = Character->FindComponentByClass<UARPGWeaponComponent>();
	const UARPGWeaponDefinition* Definition = Weapon ? Weapon->GetWeapon() : nullptr;
	const UARPGWeaponAttackTree* Tree = Definition ? Definition->AttackTree : nullptr;
	const UARPGFlinchDefinition* Flinch = Tree ? Tree->Flinch : nullptr;

	// No weapon, no tree, no flinch asset, or no clip authored for this tier all
	// mean the same thing and none of them is an error. The mechanics above have
	// already run; this is the part that is allowed to be missing.
	UAnimMontage* Montage = Flinch ? Flinch->GetMontageFor(Result) : nullptr;
	if (!Montage)
	{
		return;
	}

	if (UAnimInstance* Anim = Character->GetMesh() ? Character->GetMesh()->GetAnimInstance() : nullptr)
	{
		Anim->Montage_Play(Montage);
	}
}
