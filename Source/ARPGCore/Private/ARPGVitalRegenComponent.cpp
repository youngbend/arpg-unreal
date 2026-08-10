// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGVitalRegenComponent.h"
#include "ARPGGameplayTags.h"
#include "ARPGVitalSet.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "GameFramework/Actor.h"

UARPGVitalRegenComponent::UARPGVitalRegenComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// Nothing here replicates: the ATTRIBUTES do. Regenerating on the server and
	// letting the results replicate is what keeps a client from disagreeing with
	// the server about whether it can afford the next swing.
	SetIsReplicatedByDefault(false);
}

void UARPGVitalRegenComponent::BeginPlay()
{
	Super::BeginPlay();
	EnsureSubscribed();
}

UAbilitySystemComponent* UARPGVitalRegenComponent::GetASC() const
{
	if (!CachedASC)
	{
		CachedASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetOwner());
	}
	return CachedASC;
}

void UARPGVitalRegenComponent::EnsureSubscribed()
{
	if (bSubscribed)
	{
		return;
	}

	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC)
	{
		return;
	}

	ASC->GetGameplayAttributeValueChangeDelegate(UARPGVitalSet::GetStaminaAttribute())
		.AddUObject(this, &UARPGVitalRegenComponent::HandleStaminaChanged);

	bSubscribed = true;
}

void UARPGVitalRegenComponent::HandleStaminaChanged(const FOnAttributeChangeData& Data)
{
	// ONLY A FALL COUNTS. This component raises stamina itself, so reacting to
	// every change would restart the delay on its own regeneration and the bar
	// would never move again.
	if (Data.NewValue < Data.OldValue)
	{
		NotifyStaminaSpent();
	}
}

void UARPGVitalRegenComponent::RegenerateTowards(const FGameplayAttribute& Current,
	const FGameplayAttribute& Max, float Amount)
{
	if (Amount <= 0.f)
	{
		return;
	}

	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC)
	{
		return;
	}

	const float Maximum = ASC->GetNumericAttribute(Max);
	const float Value = ASC->GetNumericAttribute(Current);

	if (Value >= Maximum)
	{
		return;
	}

	ASC->SetNumericAttributeBase(Current, FMath::Min(Value + Amount, Maximum));
}

void UARPGVitalRegenComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	EnsureSubscribed();

	const AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority())
	{
		return;
	}

	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC)
	{
		return;
	}

	// A corpse recovers nothing. Without this a dead character quietly refills
	// while lying there and respawns at full, which hides whether the death was
	// close.
	if (ASC->HasMatchingGameplayTag(TAG_State_Dead))
	{
		return;
	}

	if (StaminaHoldTimer > 0.f)
	{
		StaminaHoldTimer = FMath::Max(0.f, StaminaHoldTimer - DeltaTime);
	}
	else
	{
		RegenerateTowards(
			UARPGVitalSet::GetStaminaAttribute(),
			UARPGVitalSet::GetMaxStaminaAttribute(),
			ASC->GetNumericAttribute(UARPGVitalSet::GetStaminaRegenRateAttribute()) * DeltaTime);
	}

	// Mana has no delay -- see the class comment. It is slow enough that this
	// costs the player nothing to ignore and fast enough to matter over a walk.
	RegenerateTowards(
		UARPGVitalSet::GetManaAttribute(),
		UARPGVitalSet::GetMaxManaAttribute(),
		ASC->GetNumericAttribute(UARPGVitalSet::GetManaRegenRateAttribute()) * DeltaTime);
}
