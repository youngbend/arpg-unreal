// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGAttackNotifies.h"
#include "ARPGCombat.h"
#include "ARPGGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Components/SkeletalMeshComponent.h"

namespace
{
	/**
	 * Raises a gameplay event on whoever owns the mesh.
	 *
	 * EventMagnitude carries the window index rather than a second tag per
	 * window: an attack's second active window differs only in which motion
	 * value it uses, and minting Event.Attack.HitboxOn.Second would double the
	 * tag vocabulary to express one integer.
	 */
	void SendAttackEvent(USkeletalMeshComponent* MeshComp, const FGameplayTag& EventTag,
		float Magnitude = 0.f)
	{
		if (!MeshComp)
		{
			return;
		}

		AActor* Owner = MeshComp->GetOwner();
		UAbilitySystemComponent* ASC =
			UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Owner);

		if (!ASC)
		{
			// Legitimate in an animation preview or on a mesh with no gameplay
			// behind it, so this is not a warning -- but it is worth being able
			// to see when a montage appears to do nothing in game.
			UE_LOG(LogARPGCombat, Verbose,
				TEXT("Attack notify '%s' on %s: no ability system component, event dropped."),
				*EventTag.ToString(), *GetNameSafe(Owner));
			return;
		}

		FGameplayEventData EventData;
		EventData.EventTag = EventTag;
		EventData.Instigator = Owner;
		EventData.Target = Owner;
		EventData.EventMagnitude = Magnitude;

		ASC->HandleGameplayEvent(EventTag, &EventData);
	}
}

// ---------------------------------------------------------------------------

UARPGAnimNotifyState_Hitbox::UARPGAnimNotifyState_Hitbox()
{
#if WITH_EDITORONLY_DATA
	NotifyColor = FColor(220, 60, 60); // red: this is the window that hurts
#endif
}

void UARPGAnimNotifyState_Hitbox::NotifyBegin(USkeletalMeshComponent* MeshComp,
	UAnimSequenceBase* Animation, float TotalDuration,
	const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyBegin(MeshComp, Animation, TotalDuration, EventReference);

	SendAttackEvent(MeshComp, TAG_Event_Attack_HitboxOn,
		bIsLandingWindow ? -1.f : static_cast<float>(WindowIndex));
}

void UARPGAnimNotifyState_Hitbox::NotifyEnd(USkeletalMeshComponent* MeshComp,
	UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	// Always paired with Begin, including when the montage is interrupted
	// mid-swing -- which is what stops a cancelled attack leaving a live hitbox
	// trailing behind the character.
	SendAttackEvent(MeshComp, TAG_Event_Attack_HitboxOff,
		bIsLandingWindow ? -1.f : static_cast<float>(WindowIndex));

	Super::NotifyEnd(MeshComp, Animation, EventReference);
}

#if WITH_EDITOR
FString UARPGAnimNotifyState_Hitbox::GetNotifyName_Implementation() const
{
	if (bIsLandingWindow)
	{
		return TEXT("Hitbox (Landing)");
	}
	return WindowIndex > 0
		? FString::Printf(TEXT("Hitbox %d"), WindowIndex + 1)
		: TEXT("Hitbox");
}
#endif

// ---------------------------------------------------------------------------

UARPGAnimNotify_ComboWindow::UARPGAnimNotify_ComboWindow()
{
#if WITH_EDITORONLY_DATA
	NotifyColor = FColor(60, 160, 220); // blue: input, not damage
#endif
}

void UARPGAnimNotify_ComboWindow::Notify(USkeletalMeshComponent* MeshComp,
	UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);

	SendAttackEvent(MeshComp, TAG_Event_Attack_ComboWindow);
}

#if WITH_EDITOR
FString UARPGAnimNotify_ComboWindow::GetNotifyName_Implementation() const
{
	return TEXT("Combo Window");
}
#endif
