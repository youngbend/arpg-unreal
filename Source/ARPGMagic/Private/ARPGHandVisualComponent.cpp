// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGHandVisualComponent.h"
#include "ARPGElementTintable.h"
#include "ARPGMagicComponent.h"
#include "ARPGMagicElement.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"

UARPGHandVisualComponent::UARPGHandVisualComponent()
{
	// Ticked as a safety net rather than as the mechanism: the selection
	// delegate is what normally drives this. A client whose magic component
	// replicated in after begin play would otherwise never bind at all, which is
	// the same late-arrival problem the poise component solves the same way.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.25f;
}

void UARPGHandVisualComponent::BeginPlay()
{
	Super::BeginPlay();
	Refresh();
}

void UARPGHandVisualComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Clear();
	Super::EndPlay(EndPlayReason);
}

void UARPGHandVisualComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	Refresh();
}

UARPGMagicComponent* UARPGHandVisualComponent::GetMagic() const
{
	return GetOwner() ? GetOwner()->FindComponentByClass<UARPGMagicComponent>() : nullptr;
}

void UARPGHandVisualComponent::HandleSelectionChanged(int32 ActiveMask)
{
	Refresh();
}

void UARPGHandVisualComponent::Refresh()
{
	UARPGMagicComponent* Magic = GetMagic();
	if (!Magic)
	{
		Clear();
		return;
	}

	// Bound on first sight rather than in BeginPlay, for the reason above.
	if (BoundMagic != Magic)
	{
		BoundMagic = Magic;
		Magic->OnSelectionChanged.AddDynamic(this, &UARPGHandVisualComponent::HandleSelectionChanged);
	}

	// The DISPLAY element: the combination where one resolved, otherwise the
	// single primitive. Fire plus water shows the steam, not two motes.
	UARPGMagicElement* Element = Magic->GetDisplayElement();

	if (Element == ShownElement)
	{
		return;
	}

	Clear();
	ShownElement = Element;

	if (!Element)
	{
		return;
	}

	// Falls back to the shared placeholder for an element whose hand effect is
	// not authored yet, which is how a brand new element is holdable the moment
	// its asset exists.
	TSubclassOf<AActor> EffectClass = Element->ResolveHandEffect();
	if (!EffectClass)
	{
		return;
	}

	AActor* Owner = GetOwner();
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}

	FActorSpawnParameters Params;
	Params.Owner = Owner;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	HandEffect = World->SpawnActor<AActor>(EffectClass, Owner->GetActorTransform(), Params);
	if (!HandEffect)
	{
		return;
	}

	// A hand effect is spawned bare -- no discharge context, no caster -- so a
	// shared placeholder has no way to look its own element up. This is how it
	// is told what colour to be. Authored effects do not implement the
	// interface and are left alone.
	ARPGElementTint::Apply(HandEffect, Element);

	const ACharacter* Character = Cast<ACharacter>(Owner);
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;

	if (Mesh && Mesh->DoesSocketExist(HandSocket))
	{
		HandEffect->AttachToComponent(Mesh,
			FAttachmentTransformRules::SnapToTargetIncludingScale, HandSocket);
	}
	else
	{
		// Badly placed beats absent: the question this answers is "did my input
		// register", and an effect at the actor's origin still answers it.
		HandEffect->AttachToActor(Owner, FAttachmentTransformRules::SnapToTargetIncludingScale);
	}
}

void UARPGHandVisualComponent::Clear()
{
	if (HandEffect)
	{
		HandEffect->Destroy();
		HandEffect = nullptr;
	}
	ShownElement = nullptr;
}
