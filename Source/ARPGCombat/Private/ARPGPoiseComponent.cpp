// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGPoiseComponent.h"
#include "ARPGReactionDefinitions.h"
#include "ARPGWeaponAttackTree.h"
#include "ARPGWeaponComponent.h"
#include "ARPGWeaponDefinition.h"
#include "Animation/AnimInstance.h"
#include "GameFramework/Character.h"
#include "ARPGAttributeLibrary.h"
#include "ARPGGameplayTags.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffectTypes.h"

UARPGPoiseComponent::UARPGPoiseComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	// Starts enabled ONLY to let EnsureSubscribed retry for a late PlayerState;
	// once the meter is empty and the subscription is live, RefreshTickState
	// switches it off and a hit switches it back on.
	PrimaryComponentTick.bStartWithTickEnabled = true;
	SetIsReplicatedByDefault(false); // resolved server-side; the Poise attribute replicates
}

void UARPGPoiseComponent::RefreshTickState()
{
	const bool bNeedsTick = !PoiseDamageHandle.IsValid()   // still waiting on an ASC
		|| BreakImmunityTimer > 0.f
		|| HoldTimer > 0.f
		|| GetCurrentPoise() > 0.f;

	SetComponentTickEnabled(bNeedsTick);
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

		// Bound at last: nothing else needs a tick until a hit lands.
		RefreshTickState();
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

float UARPGPoiseComponent::GetCurrentPoise() const
{
	// The BASE value, to match SetPoise's write. Reading the current value and
	// writing the base is what folds any active poise modifier into the base --
	// see UARPGAttributeLibrary.
	return UARPGAttributeLibrary::GetBase(GetASC(), UARPGVitalSet::GetPoiseAttribute());
}

float UARPGPoiseComponent::GetMaxPoise() const
{
	// The maximum is a ceiling that buffs legitimately raise, so the CURRENT
	// value is the right one here -- the asymmetry with GetCurrentPoise above is
	// deliberate.
	const float Max = UARPGAttributeLibrary::GetCurrent(GetASC(), UARPGVitalSet::GetMaxPoiseAttribute());
	return Max > 0.f ? Max : 100.f;
}

void UARPGPoiseComponent::SetPoise(float NewValue)
{
	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC)
	{
		return;
	}

	UARPGAttributeLibrary::SetBase(ASC, UARPGVitalSet::GetPoiseAttribute(),
		FMath::Clamp(NewValue, 0.f, GetMaxPoise()));

	OnPoiseChanged.Broadcast(GetCurrentPoise(), GetMaxPoise());
	RefreshTickState();
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
		RefreshTickState();

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

	// Covers the ASC arriving after BeginPlay -- a player's lives on the
	// PlayerState. Retried on a slow cadence rather than every frame: it is a
	// global lookup plus an attribute-set search, and on an actor that will never
	// have an ability system it would otherwise run forever.
	if (!PoiseDamageHandle.IsValid())
	{
		SubscribeRetryTimer -= DeltaTime;
		if (SubscribeRetryTimer <= 0.f)
		{
			SubscribeRetryTimer = 0.5f;
			EnsureSubscribed();
		}
	}

	if (BreakImmunityTimer > 0.f)
	{
		BreakImmunityTimer = FMath::Max(0.f, BreakImmunityTimer - DeltaTime);
	}

	const float Current = GetCurrentPoise();
	if (Current <= 0.f)
	{
		RefreshTickState();
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

	UAbilitySystemComponent* ASC = GetASC();
	UAnimInstance* Anim = Character->GetMesh() ? Character->GetMesh()->GetAnimInstance() : nullptr;
	if (!Anim)
	{
		return;
	}

	// Through the ability system, which replicates the montage to every client
	// via its own RepAnimMontageInfo. A bare Anim->Montage_Play here plays only
	// on the machine that called it -- and poise resolves inside
	// PostGameplayEffectExecute, which for a hitbox-applied hit is the server
	// alone. Every flinch and stance break in the game was server-only.
	//
	// PlayMontage needs the actor info wired up, which is true for anything with
	// an initialised ASC; the direct call remains the fallback for a fixture that
	// has an anim instance but no ability system.
	if (ASC && ASC->AbilityActorInfo.IsValid() && ASC->AbilityActorInfo->AnimInstance.IsValid())
	{
		ASC->PlayMontage(nullptr, FGameplayAbilityActivationInfo(), Montage, 1.f);
		return;
	}

	Anim->Montage_Play(Montage);
}
