// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGGameplayAbility_Imbue.h"
#include "ARPGElementTintable.h"
#include "ARPGElementalCoating.h"
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

UARPGHitboxComponent* UARPGGameplayAbility_Imbue::ResolveElementalHitbox(
	UARPGHitboxComponent* SwingHitbox)
{
	if (ElementalHitbox)
	{
		return ElementalHitbox;
	}

	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !SwingHitbox)
	{
		return nullptr;
	}

	// An authored one wins. A character given a coating hitbox was given it for a
	// reason -- its own socket, a shape tuned to the silhouette -- and generating
	// one anyway would throw that work away. bAllowFallback is false because
	// there is nothing to fall back TO: borrowing the weapon's own hitbox would
	// mean two abilities arming the same component every swing.
	if (UARPGHitboxComponent* Authored = UARPGHitboxComponent::FindOnActor(
			Avatar, EARPGHitboxSource::Elemental, /*bAllowFallback=*/false))
	{
		ElementalHitbox = Authored;
		bOwnsElementalHitbox = false;
		return ElementalHitbox;
	}

	// Otherwise make one. Imbuing has to work on any character that can swing,
	// not only on those whose content has been revisited -- and the sensible
	// default shape is the swing's own, scaled.
	UARPGHitboxComponent* Created = NewObject<UARPGHitboxComponent>(Avatar);
	Created->HitboxSource = EARPGHitboxSource::Elemental;
	Created->RegisterComponent();

	// Attached to the SWING'S hitbox rather than to a socket: a coating follows
	// the blade, and whatever the attack chose to swing -- blade or boot -- is
	// exactly what it should follow, at whatever offset that hitbox carries.
	Created->AttachToComponent(SwingHitbox,
		FAttachmentTransformRules::SnapToTargetIncludingScale);

	ElementalHitbox = Created;
	bOwnsElementalHitbox = true;
	return ElementalHitbox;
}

bool UARPGGameplayAbility_Imbue::ArmElementalHitbox(UARPGHitboxComponent* SwingHitbox,
	float MotionValue)
{
	if (!SwingHitbox || !ImbuedElement)
	{
		return false;
	}

	AActor* Avatar = GetAvatarActorFromActorInfo();
	const UARPGMagicComponent* Magic =
		Avatar ? Avatar->FindComponentByClass<UARPGMagicComponent>() : nullptr;
	if (!Magic)
	{
		return false;
	}

	UARPGHitboxComponent* Elemental = ResolveElementalHitbox(SwingHitbox);
	if (!Elemental)
	{
		return false;
	}

	// Rebuilt per window rather than once per imbue: the damage is scaled by the
	// window's motion value, and a two-window swing hits for different amounts.
	Magic->BuildImbueCoating(ImbuedElement, MotionValue).ApplyTo(Elemental, *SwingHitbox);

	// The swing belongs to the character, whichever hitbox delivers it.
	Elemental->SetSourceActor(Avatar);

	// Paired for the length of the window. Without this the wider elemental sweep
	// and the weapon's own sweep would knock each other out on any target with
	// i-frames -- see UARPGHitboxComponent::PairWithSwingPartner.
	Elemental->PairWithSwingPartner(SwingHitbox);
	Elemental->ActivateHitbox();

	return true;
}

void UARPGGameplayAbility_Imbue::DisarmElementalHitbox()
{
	if (!ElementalHitbox)
	{
		return;
	}

	ElementalHitbox->DeactivateHitbox();

	// Unpaired as well as disarmed, so the weapon's hitbox is not left holding a
	// partner that is no longer part of the swing it is about to make next.
	ElementalHitbox->ClearSwingPartner();
}

void UARPGGameplayAbility_Imbue::ReleaseElementalHitbox()
{
	if (!ElementalHitbox)
	{
		return;
	}

	DisarmElementalHitbox();

	if (bOwnsElementalHitbox)
	{
		ElementalHitbox->DestroyComponent();
	}

	ElementalHitbox = nullptr;
	bOwnsElementalHitbox = false;
}

void UARPGGameplayAbility_Imbue::ArmSwingAugment_Implementation(UARPGHitboxComponent* SwingHitbox,
	float MotionValue)
{
	if (ArmElementalHitbox(SwingHitbox, MotionValue))
	{
		bDelivered = true;
	}
}

void UARPGGameplayAbility_Imbue::DisarmSwingAugment_Implementation()
{
	DisarmElementalHitbox();
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
	// leave a coating actor stuck on the weapon -- nor a live elemental hitbox
	// still sweeping behind a swing that was cut short.
	if (ImbueEffect)
	{
		ImbueEffect->Destroy();
		ImbueEffect = nullptr;
	}

	ReleaseElementalHitbox();

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
