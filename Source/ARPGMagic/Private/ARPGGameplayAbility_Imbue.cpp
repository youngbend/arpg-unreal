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

	AbilityTags.AddTag(TAG_Ability_Imbue);

	ActivationBlockedTags.AddTag(TAG_State_Flinching);
	ActivationBlockedTags.AddTag(TAG_State_StanceBroken);
	ActivationBlockedTags.AddTag(TAG_State_Dead);
}

void UARPGGameplayAbility_Imbue::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

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

	// The ability stays alive holding the imbue until it is spent or expires --
	// which is why it is InstancedPerActor rather than per-execution: the melee
	// ability has to be able to find it and ask what the weapon is coated in.
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

void UARPGGameplayAbility_Imbue::ApplyToHitbox(UARPGHitboxComponent* Hitbox, float MotionValue)
{
	if (!Hitbox || !ImbuedElement)
	{
		return;
	}

	AActor* Avatar = GetAvatarActorFromActorInfo();
	const UARPGMagicComponent* Magic =
		Avatar ? Avatar->FindComponentByClass<UARPGMagicComponent>() : nullptr;
	if (!Magic)
	{
		return;
	}

	Hitbox->BaseDamage = Magic->GetImbueDamage(ImbuedElement, MotionValue);
	Hitbox->PoiseDamage = Magic->GetImbuePoiseDamage(ImbuedElement, MotionValue);
	Hitbox->DamageType = ImbuedElement->DamageType;
	Hitbox->MagicElementTag = ImbuedElement->ElementTag;

	if (ImbuedElement->OnHitEffect)
	{
		Hitbox->OnHitEffects.AddUnique(ImbuedElement->OnHitEffect);
		Hitbox->OnHitEffectDuration = ImbuedElement->StatusDuration;
	}

	Hitbox->SetSourceActor(Avatar);
}

void UARPGGameplayAbility_Imbue::ConsumeImbue()
{
	if (!ImbuedElement)
	{
		return;
	}

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

	if (const AActor* Avatar = GetAvatarActorFromActorInfo())
	{
		if (UWorld* World = Avatar->GetWorld())
		{
			World->GetTimerManager().ClearTimer(ExpiryTimer);
		}
	}

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}
