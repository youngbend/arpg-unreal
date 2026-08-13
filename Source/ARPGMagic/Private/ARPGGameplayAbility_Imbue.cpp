// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGGameplayAbility_Imbue.h"
#include "ARPGElementTintable.h"
#include "ARPGGameplayTags.h"
#include "ARPGHitboxComponent.h"
#include "ARPGMagic.h"
#include "ARPGMagicComponent.h"
#include "ARPGMagicElement.h"
#include "GameFramework/Character.h"
#include "GameplayEffect.h"
#include "TimerManager.h"

UARPGGameplayAbility_Imbue::UARPGGameplayAbility_Imbue()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;

	{
		// SetAssetTags replaces rather than appends, and it is protected --
		// so a subclass carries the base class's tags forward itself.
		FGameplayTagContainer Tags = GetAssetTags();
		Tags.AddTag(TAG_Ability_Imbue);
		SetAssetTags(Tags);
	}

	ActivationBlockedTags.AddTag(TAG_State_Flinching);
	ActivationBlockedTags.AddTag(TAG_State_StanceBroken);
	ActivationBlockedTags.AddTag(TAG_State_Dead);
}

void UARPGGameplayAbility_Imbue::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	bDelivered = false;

	AActor* Avatar = GetAvatarActorFromActorInfo();
	UARPGMagicComponent* Magic = Avatar ? Avatar->FindComponentByClass<UARPGMagicComponent>() : nullptr;

	if (!Magic)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// The spend is server-authoritative -- ConsumeForImbue takes mana. A client
	// predicting it would let a player imbue with mana they do not have.
	if (!HasAuthority(&ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
		return;
	}

	ImbuedElement = Magic->ConsumeForImbue();
	if (!ImbuedElement)
	{
		// Nothing readied, or the mana was not there. ConsumeForImbue has already
		// raised OnSpellFailed for the second case.
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	SpawnImbueEffect();

	UE_LOG(LogARPGMagic, Log, TEXT("%s imbued with '%s'"),
		*GetNameSafe(Avatar), *ImbuedElement->ElementTag.ToString());

	// The ability stays alive holding the imbue until a swing spends it or it
	// expires -- which is why it is InstancedPerActor rather than per-execution:
	// the live instance IS the coating, and ARPGSwingAugments::FindActive has to
	// be able to find it on the ASC while the next attack is arming.
	//
	// With ImbueDuration at its default of 0 there is no timer and the coating
	// simply waits for the next attack, however long that takes. That is the
	// authored behaviour, not a leak: NotifySwingEnded is what normally ends it.
	if (ImbueDuration > 0.f && Avatar->GetWorld())
	{
		Avatar->GetWorld()->GetTimerManager().SetTimer(ExpiryTimer, [this]()
		{
			ConsumeImbue();
		}, ImbueDuration, false);
	}
}

void UARPGGameplayAbility_Imbue::SpawnImbueEffect()
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!ImbuedElement || !Avatar || !Avatar->GetWorld())
	{
		return;
	}

	TSubclassOf<AActor> EffectClass = ImbuedElement->ResolveImbueEffect();
	if (!EffectClass)
	{
		return;
	}

	FActorSpawnParameters Params;
	Params.Owner = Avatar;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ImbueEffect = Avatar->GetWorld()->SpawnActor<AActor>(
		EffectClass, Avatar->GetActorTransform(), Params);

	if (!ImbueEffect)
	{
		return;
	}

	ARPGElementTint::Apply(ImbueEffect, ImbuedElement);

	// Attached to the weapon socket where there is one, so the coating tracks the
	// blade rather than the character's origin.
	if (const ACharacter* Character = Cast<ACharacter>(Avatar))
	{
		if (USkeletalMeshComponent* Mesh = Character->GetMesh())
		{
			ImbueEffect->AttachToComponent(Mesh,
				FAttachmentTransformRules::SnapToTargetIncludingScale,
				TEXT("weapon_r"));
			return;
		}
	}

	ImbueEffect->AttachToActor(Avatar, FAttachmentTransformRules::SnapToTargetIncludingScale);
}

bool UARPGGameplayAbility_Imbue::ApplyToHitbox(UARPGHitboxComponent* Hitbox, float MotionValue)
{
	if (!Hitbox || !ImbuedElement)
	{
		return false;
	}

	const AActor* Avatar = GetAvatarActorFromActorInfo();
	const UARPGMagicComponent* Magic =
		Avatar ? Avatar->FindComponentByClass<UARPGMagicComponent>() : nullptr;
	if (!Magic)
	{
		return false;
	}

	// Attribution and the physical payload are the attack's business and are
	// already set; this adds the elemental half beside them and touches nothing
	// else on the hitbox.
	Hitbox->SetElementalRider(Magic->BuildImbueRider(ImbuedElement, MotionValue));
	return true;
}

void UARPGGameplayAbility_Imbue::ArmSwingAugment_Implementation(UARPGHitboxComponent* Hitbox,
	float MotionValue)
{
	if (ApplyToHitbox(Hitbox, MotionValue))
	{
		bDelivered = true;
	}
}

void UARPGGameplayAbility_Imbue::NotifySwingEnded_Implementation()
{
	if (bDelivered)
	{
		ConsumeImbue();
	}
}

void UARPGGameplayAbility_Imbue::ConsumeImbue()
{
	if (!ImbuedElement)
	{
		return;
	}

	// Ending the ability is what drops the coating: EndAbility clears the element,
	// destroys the weapon effect and cancels the expiry timer, so there is exactly
	// one teardown path whether the imbue was spent, timed out or interrupted.
	ImbuedElement = nullptr;
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UARPGGameplayAbility_Imbue::EndAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility, bool bWasCancelled)
{
	// Runs on every exit including cancellation, so an interrupted imbue cannot
	// leave a coating actor stuck on the weapon.
	if (ImbueEffect)
	{
		ImbueEffect->Destroy();
		ImbueEffect = nullptr;
	}

	ImbuedElement = nullptr;
	bDelivered = false;

	if (const AActor* Avatar = GetAvatarActorFromActorInfo())
	{
		if (UWorld* World = Avatar->GetWorld())
		{
			World->GetTimerManager().ClearTimer(ExpiryTimer);
		}
	}

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}
