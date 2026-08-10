// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGCloakComponent.h"
#include "ARPGElementTintable.h"
#include "ARPGMagic.h"
#include "ARPGMagicElement.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Net/UnrealNetwork.h"

UARPGCloakComponent::UARPGCloakComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	SetIsReplicatedByDefault(true);
}

void UARPGCloakComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UARPGCloakComponent, ActiveElement);
	DOREPLIFETIME(UARPGCloakComponent, ShieldRemaining);
}

void UARPGCloakComponent::ApplyCloak(const FARPGDischargeContext& Context)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	UARPGMagicElement* Element = Context.PrimaryElement;
	if (!Element)
	{
		return;
	}

	// Re-cloaking replaces rather than stacks: two cloaks at once has no meaning,
	// and leaving the old one running would leak its effect actor.
	if (IsCloaked())
	{
		EndCloak();
	}

	ActiveElement = Element;

	const float BaseDuration = Element->CloakBaseDuration > 0.f
		? Element->CloakBaseDuration
		: DefaultDuration;
	CloakTimer = BaseDuration + Element->CloakChargeBonus * Context.Charge;

	// Shield capacity is the same number the spell would have dealt as damage --
	// see the class comment.
	ShieldRemaining = (DefensiveElementTag.IsValid()
		&& Element->ElementTags.HasTagExact(DefensiveElementTag))
		? Context.ComputedDamage
		: 0.f;

	if (TSubclassOf<AActor> EffectClass = Element->ResolveDischargeEffect(EARPGDischargeType::Cloak))
	{
		FActorSpawnParameters Params;
		Params.Owner = GetOwner();
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		CloakEffect = GetWorld()->SpawnActor<AActor>(
			EffectClass, GetOwner()->GetActorTransform(), Params);

		if (CloakEffect)
		{
			// Spawned directly rather than through the discharge ability, so the
			// tint has to happen here too -- a cloak is the one discharge whose
			// lifetime belongs to this component instead of to the effect.
			ARPGElementTint::Apply(CloakEffect, Element);

			CloakEffect->AttachToActor(GetOwner(),
				FAttachmentTransformRules::SnapToTargetIncludingScale);
		}
	}

	SelfEffectTimer = 0.f;
	ApplySelfEffect();

	OnCloakApplied.Broadcast(ActiveElement);

	UE_LOG(LogARPGMagic, Log, TEXT("%s cloaked in '%s' for %.1fs (shield %.0f)"),
		*GetNameSafe(GetOwner()), *Element->ElementTag.ToString(), CloakTimer, ShieldRemaining);
}

void UARPGCloakComponent::ApplySelfEffect()
{
	if (!ActiveElement || !ActiveElement->CloakSelfEffect)
	{
		return;
	}

	UAbilitySystemComponent* ASC =
		UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetOwner());
	if (!ASC)
	{
		return;
	}

	// Self-inflicted: the caster is both source and target, which is exactly what
	// a fire cloak burning its wearer means.
	FGameplayEffectContextHandle ContextHandle = ASC->MakeEffectContext();
	ContextHandle.AddSourceObject(this);

	const FGameplayEffectSpecHandle Spec =
		ASC->MakeOutgoingSpec(ActiveElement->CloakSelfEffect, 1.f, ContextHandle);
	if (Spec.IsValid())
	{
		ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data);
	}

	SelfEffectTimer = SelfEffectInterval;
}

void UARPGCloakComponent::BreakCloak()
{
	if (!IsCloaked() || !GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}
	EndCloak();
}

void UARPGCloakComponent::EndCloak()
{
	if (CloakEffect)
	{
		CloakEffect->Destroy();
		CloakEffect = nullptr;
	}

	ActiveElement = nullptr;
	CloakTimer = 0.f;
	ShieldRemaining = 0.f;
	SelfEffectTimer = 0.f;

	OnCloakEnded.Broadcast(nullptr);
}

float UARPGCloakComponent::AbsorbDamage(float Incoming)
{
	if (ShieldRemaining <= 0.f || Incoming <= 0.f)
	{
		return 0.f;
	}

	const float Absorbed = FMath::Min(ShieldRemaining, Incoming);
	ShieldRemaining -= Absorbed;

	OnShieldAbsorbed.Broadcast(Absorbed);

	// A spent shield does NOT end the cloak. The two have separate lifetimes on
	// purpose: the element's other effects, and its look, run for the full
	// duration whether or not the shield held.
	return Absorbed;
}

void UARPGCloakComponent::OnRep_ActiveElement()
{
	if (ActiveElement)
	{
		OnCloakApplied.Broadcast(ActiveElement);
	}
	else
	{
		OnCloakEnded.Broadcast(nullptr);
	}
}

void UARPGCloakComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!IsCloaked() || !GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	CloakTimer -= DeltaTime;
	if (CloakTimer <= 0.f)
	{
		EndCloak();
		return;
	}

	if (SelfEffectInterval > 0.f)
	{
		SelfEffectTimer -= DeltaTime;
		if (SelfEffectTimer <= 0.f)
		{
			ApplySelfEffect();
		}
	}
}
