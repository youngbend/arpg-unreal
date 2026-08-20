// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGStatusVfxSubsystem.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "ARPGCombat.h"
#include "ARPGGameplayTags.h"
#include "ARPGGeometryProbe.h"
#include "ARPGStatusEffectComponent.h"
#include "ARPGVfxFittable.h"
#include "AbilitySystemComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"

bool UARPGStatusVfxSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// Nothing here affects gameplay, so a machine with no one looking should not
	// be spending anything on it. A listen server still creates it -- somebody is
	// watching on that machine.
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->GetNetMode() != NM_DedicatedServer;
}

void UARPGStatusVfxSubsystem::Deinitialize()
{
	for (TPair<FARPGStatusVfxKey, FARPGStatusVfxRequest>& Entry : Requests)
	{
		Release(Entry.Value);
	}
	Requests.Reset();
	Registered.Reset();

	Super::Deinitialize();
}

TStatId UARPGStatusVfxSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UARPGStatusVfxSubsystem, STATGROUP_Tickables);
}

void UARPGStatusVfxSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	Timer -= DeltaTime;
	if (Timer > 0.f)
	{
		return;
	}
	Timer = ReevaluateInterval;

	Reconcile();
}

void UARPGStatusVfxSubsystem::RegisterAbilitySystem(UAbilitySystemComponent* ASC)
{
	if (ASC && !Registered.Contains(ASC))
	{
		Registered.Add(ASC);
	}
}

void UARPGStatusVfxSubsystem::UnregisterAbilitySystem(UAbilitySystemComponent* ASC)
{
	Registered.RemoveAllSwap([ASC](const TWeakObjectPtr<UAbilitySystemComponent>& Entry)
	{
		return !Entry.IsValid() || Entry.Get() == ASC;
	});
}

// ---------------------------------------------------------------------------
// Gathering
// ---------------------------------------------------------------------------

void UARPGStatusVfxSubsystem::GatherRequests()
{
	for (TPair<FARPGStatusVfxKey, FARPGStatusVfxRequest>& Entry : Requests)
	{
		Entry.Value.bSeen = false;
	}

	for (int32 Index = Registered.Num() - 1; Index >= 0; --Index)
	{
		UAbilitySystemComponent* ASC = Registered[Index].Get();
		if (!ASC)
		{
			Registered.RemoveAtSwap(Index);
			continue;
		}

		AActor* Target = ASC->GetAvatarActor();
		if (!Target)
		{
			// A player's ability system lives on the PlayerState and is bound to
			// the pawn later, so this is a normal transient state rather than an
			// error -- the next pass picks it up.
			continue;
		}

		// UNFILTERED, deliberately. Filtering on the Status.* owning tag would be
		// cheaper, but it assumes every status effect also grants its own identity
		// tag -- and what actually makes an effect a status here is carrying a
		// UARPGStatusEffectComponent, which no FGameplayEffectQuery can express.
		// An effect with presentation data and no tag would silently lose its
		// visual, which is a worse trade than the array this allocates.
		const TArray<FActiveGameplayEffectHandle> Handles =
			ASC->GetActiveEffects(FGameplayEffectQuery());

		for (const FActiveGameplayEffectHandle& Handle : Handles)
		{
			const UGameplayEffect* Effect = ASC->GetGameplayEffectDefForHandle(Handle);
			if (!Effect)
			{
				continue;
			}

			const UARPGStatusEffectComponent* Presentation =
				Effect->FindComponent<UARPGStatusEffectComponent>();

			// No presentation data, or an effect that deliberately has no
			// visual. Plenty of statuses want none, which is not a misconfigured
			// asset and must not warn.
			if (!Presentation || Presentation->VfxActorClass.IsNull())
			{
				continue;
			}

			FARPGStatusVfxKey Key;
			Key.TargetKey = FObjectKey(Target);
			Key.StatusTag = Presentation->StatusTag;

			const int32 Stacks = FMath::Max(1, ASC->GetCurrentStackCount(Handle));

			FARPGStatusVfxRequest& Request = Requests.FindOrAdd(Key);
			Request.Target = Target;
			Request.Presentation = Presentation;
			Request.bSeen = true;

			// A stack change reaches a LIVE visual without respawning it, so a
			// fire grows as it is fed rather than restarting each time.
			if (Request.Stacks != Stacks)
			{
				Request.Stacks = Stacks;
				AActor* Instance = Request.Instance.Get();
				if (Instance && Instance->Implements<UARPGVfxFittable>())
				{
					IARPGVfxFittable::Execute_SetStacks(Instance, Stacks);
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Reconciliation
// ---------------------------------------------------------------------------

void UARPGStatusVfxSubsystem::Reconcile()
{
	GatherRequests();

	const FVector Anchor = GetAnchorLocation();

	struct FCandidate
	{
		FARPGStatusVfxKey Key;
		int32 Priority = 0;
		double DistanceSq = 0.0;
	};

	TArray<FCandidate> Candidates;
	Candidates.Reserve(Requests.Num());

	TArray<FARPGStatusVfxKey> Doomed;

	for (TPair<FARPGStatusVfxKey, FARPGStatusVfxRequest>& Entry : Requests)
	{
		FARPGStatusVfxRequest& Request = Entry.Value;
		AActor* Target = Request.Target.Get();
		const UARPGStatusEffectComponent* Presentation = Request.Presentation.Get();

		// The effect ended, or the thing carrying it is gone. Dropped entirely
		// rather than merely un-realised: there is nothing left to want it.
		if (!Request.bSeen || !Target || !Presentation)
		{
			Release(Request);
			Doomed.Add(Entry.Key);
			continue;
		}

		const double DistanceSq = FVector::DistSquared(Target->GetActorLocation(), Anchor);
		if (DistanceSq > static_cast<double>(CullDistance) * CullDistance)
		{
			// Released but KEPT. Walking back towards a distant fire should show
			// it again, which means the request has to outlive the visual.
			Release(Request);
			continue;
		}

		FCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		Candidate.Key = Entry.Key;
		Candidate.Priority = Presentation->VfxPriority;
		Candidate.DistanceSq = DistanceSq;
	}

	for (const FARPGStatusVfxKey& Key : Doomed)
	{
		Requests.Remove(Key);
	}

	// Highest priority first, nearest first within a priority. A character on
	// fire matters more than the same character's faint Weakened shimmer, and
	// between two fires the one you can see wins.
	Candidates.Sort([](const FCandidate& A, const FCandidate& B)
	{
		if (A.Priority != B.Priority)
		{
			return A.Priority > B.Priority;
		}
		return A.DistanceSq < B.DistanceSq;
	});

	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		FARPGStatusVfxRequest* Request = Requests.Find(Candidates[Index].Key);
		if (!Request)
		{
			continue;
		}

		const bool bAfford = Index < MaxInstances;

		if (bAfford && !Request->Instance.IsValid())
		{
			if (AActor* Target = Request->Target.Get())
			{
				Realise(*Request, Target);
			}
		}
		else if (!bAfford && Request->Instance.IsValid())
		{
			// Released rather than left stale, so the budget genuinely bounds
			// what is alive rather than only what is newly spawned.
			Release(*Request);
		}
	}
}

void UARPGStatusVfxSubsystem::Realise(FARPGStatusVfxRequest& Request, AActor* Target)
{
	const UARPGStatusEffectComponent* Presentation = Request.Presentation.Get();
	if (!Presentation)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// THE CHEAP ONE FIRST. Most status visuals are a single emitter, and wrapping
	// each in an actor purely so something can be spawned is a class per status
	// that holds one component and does nothing. Attached rather than spawned, so
	// it follows the target with no placement pass and no per-frame anything.
	if (UNiagaraSystem* System = Presentation->VfxSystem.LoadSynchronous())
	{
		UNiagaraComponent* Attached = UNiagaraFunctionLibrary::SpawnSystemAttached(
			System, Target->GetRootComponent(), NAME_None,
			FVector::ZeroVector, FRotator::ZeroRotator,
			EAttachLocation::SnapToTarget, /*bAutoDestroy=*/false);

		if (Attached)
		{
			// FITTED THE SAME WAY THE ACTOR PATH IS, so a burning tree is alight at
			// its own size whichever kind of visual it named. Fitting is a
			// measurement of the TARGET; the two paths differ only in what they
			// apply the answer to.
			ARPGGeometryProbe::FitVfxComponent(Attached, Target,
				Presentation->VfxFit, Presentation->VfxScale);

			Request.Attached = Attached;
			return;
		}
	}

	// Synchronous, and deliberately so: this runs at most a few times a second
	// behind a hard budget, and an async load would leave the visual arriving
	// after a short affliction had already ended.
	UClass* VfxClass = Presentation->VfxActorClass.LoadSynchronous();
	if (!VfxClass)
	{
		UE_LOG(LogARPGCombat, Warning,
			TEXT("Status '%s' names a VFX class that failed to load; it will show nothing."),
			*Presentation->StatusTag.ToString());
		return;
	}

	FActorSpawnParameters Params;
	Params.Owner = Target;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* Instance = World->SpawnActor<AActor>(VfxClass, Target->GetActorTransform(), Params);
	if (!Instance)
	{
		return;
	}

	Request.Instance = Instance;

	// Placement, scaling and the fit handshake all live in the shared probe, so
	// a status on a burning tree is placed by exactly the same rules as anything
	// else that fits a visual to a thing.
	ARPGGeometryProbe::FitVfxToTarget(Instance, Target, Presentation->VfxFit,
		Presentation->VfxScale, Request.Stacks);
}

void UARPGStatusVfxSubsystem::Release(FARPGStatusVfxRequest& Request)
{
	// An attached system deactivates rather than being destroyed outright, which
	// is Niagara's own fade: emission stops and whatever is already in flight
	// finishes its life. The same distinction the actor path makes below, for
	// free.
	if (UNiagaraComponent* Attached = Request.Attached.Get())
	{
		Request.Attached = nullptr;

		Attached->Deactivate();
		Attached->SetAutoDestroy(true);
		return;
	}

	AActor* Instance = Request.Instance.Get();
	Request.Instance = nullptr;

	if (!Instance)
	{
		return;
	}

	// Offered a fade-out first. Particles already in flight vanishing mid-air is
	// the difference between a fire going out and a fire being switched off. An
	// implementer destroys itself once it has faded.
	if (Instance->Implements<UARPGVfxFittable>())
	{
		IARPGVfxFittable::Execute_Finish(Instance);
		return;
	}

	Instance->Destroy();
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

FVector UARPGStatusVfxSubsystem::GetAnchorLocation() const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return FVector::ZeroVector;
	}

	// The local view, not a configured anchor node. Culling is about what THIS
	// machine can see, and a listen server's own view is the right centre for
	// its own budget -- each machine spends its own.
	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		if (const AActor* ViewTarget = PC->GetViewTarget())
		{
			return ViewTarget->GetActorLocation();
		}
	}

	return FVector::ZeroVector;
}

FARPGStatusVfxStats UARPGStatusVfxSubsystem::GetStats() const
{
	FARPGStatusVfxStats Stats;
	Stats.Requests = Requests.Num();
	Stats.Registered = Registered.Num();
	Stats.Budget = MaxInstances;

	for (const TPair<FARPGStatusVfxKey, FARPGStatusVfxRequest>& Entry : Requests)
	{
		if (Entry.Value.Instance.IsValid())
		{
			++Stats.Live;
		}
	}

	return Stats;
}
