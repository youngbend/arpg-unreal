// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectKey.h"
#include "ARPGStatusVfxSubsystem.generated.h"

class UNiagaraComponent;

struct FGameplayEffectQuery;

class UAbilitySystemComponent;
class UARPGStatusEffectComponent;

/**
 * One afflicted (target, status) pair that wants a visual.
 *
 * Exists whether or not the budget currently affords it one -- which is the
 * point: wanting and having are separate, so a fire that loses its visual to
 * something more important gets it back when that thing goes away, rather than
 * being forgotten.
 */
USTRUCT()
struct FARPGStatusVfxRequest
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TWeakObjectPtr<AActor> Target;

	/** The presentation data, which lives on the GameplayEffect's own CDO. */
	UPROPERTY(Transient)
	TWeakObjectPtr<const UARPGStatusEffectComponent> Presentation;

	UPROPERTY(Transient)
	TWeakObjectPtr<AActor> Instance;

	/** The attached system, when the status named one instead of an actor. */
	UPROPERTY(Transient)
	TWeakObjectPtr<UNiagaraComponent> Attached;

	int32 Stacks = 1;

	/** Cleared each pass and re-set by anything still afflicted; see Reconcile. */
	bool bSeen = false;
};

/**
 * Keyed per target AND per status, so one burning thing can also be shocked.
 *
 * FObjectKey, not GetUniqueID(). That returns the object's internal index, which
 * the engine RECYCLES after garbage collection -- so a request left behind by a
 * destroyed actor could collide with a freshly spawned one and hand it somebody
 * else's visual. FObjectKey pairs the index with a serial number precisely to
 * make that impossible.
 */
USTRUCT()
struct FARPGStatusVfxKey
{
	GENERATED_BODY()

	FObjectKey TargetKey;

	UPROPERTY()
	FGameplayTag StatusTag;

	bool operator==(const FARPGStatusVfxKey& Other) const
	{
		return TargetKey == Other.TargetKey && StatusTag == Other.StatusTag;
	}
};

FORCEINLINE uint32 GetTypeHash(const FARPGStatusVfxKey& Key)
{
	// Unqualified on purpose. FGameplayTag declares its hash as a hidden friend,
	// which qualified lookup cannot see -- only argument-dependent lookup can.
	return HashCombine(GetTypeHash(Key.TargetKey), GetTypeHash(Key.StatusTag));
}

/** Counts for tests and the debug overlay. */
USTRUCT(BlueprintType)
struct FARPGStatusVfxStats
{
	GENERATED_BODY()

	/** Afflictions that want a visual, affordable or not. */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|VFX")
	int32 Requests = 0;

	/** Visuals actually spawned right now. */
	UPROPERTY(BlueprintReadOnly, Category = "ARPG|VFX")
	int32 Live = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ARPG|VFX")
	int32 Registered = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ARPG|VFX")
	int32 Budget = 0;
};

/**
 * Spawns and retires the visual for every active status effect in the world,
 * fitting one authored asset to whatever it lands on. Port of Godot's
 * StatusVfxManager, deferred out of phase 2 as presentation.
 *
 * WHY THIS EXISTS RATHER THAN A VFX PER OBJECT TYPE. The naive shape of the
 * feature is an asset per (effect x silhouette): a burning tree, a burning
 * barrel, a burning rat. That is an N x M authoring cost that grows whenever
 * either axis does, and it is wrong about where the variation lives. What
 * differs between a burning tree and a burning rat is the BOUNDS of the thing
 * alight; what differs between fire and corruption is the LOOK of the medium.
 * Those are orthogonal, so bounds are measured at runtime and only the look is
 * authored -- one asset per effect, fitted to anything. See ARPGGeometryProbe.
 *
 * THE BUDGET LIVES HERE for the same reason the spread simulation does not own
 * it: particle systems and lights are the real cost of a world-scale fire, and
 * a cap only means something if it is global across every effect and every
 * afflicted thing at once. Over budget, highest priority wins and nearest wins
 * within a priority.
 *
 * **This POLLS rather than subscribing, and that is a deliberate departure from
 * the Godot original.** That version connected to each combat component's
 * applied/removed/expired signals to maintain its request map, then reconciled
 * the map on a timer anyway. Keeping both halves means a missed or double-fired
 * signal leaves a visual that never retires, with nothing to correct it. Since
 * the reconcile pass already runs, deriving the whole map from the ability
 * system's live effect list each pass removes the bookkeeping AND the entire
 * class of desync -- for the cost of noticing an affliction up to
 * ReevaluateInterval late, which for a fire lighting is not a cost at all.
 *
 * Purely cosmetic. Nothing here runs on a dedicated server, and nothing gameplay
 * reads its output.
 */
UCLASS()
class ARPGCOMBAT_API UARPGStatusVfxSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ USubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;
	//~ End USubsystem

	//~ FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject

	/**
	 * Called by UARPGAbilitySystemComponent on begin play.
	 *
	 * Registration rather than a world sweep: asking every actor in a streamed
	 * level what effects it carries, four times a second, would cost far more
	 * than the visuals. Since every ability system in the project is one of
	 * ours, one line in that class covers everything without a word of
	 * per-object authoring -- which was the original's best property.
	 */
	void RegisterAbilitySystem(UAbilitySystemComponent* ASC);
	void UnregisterAbilitySystem(UAbilitySystemComponent* ASC);

	/** Rebuilds requests and re-spends the budget. Normally on a timer. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|VFX")
	void Reconcile();

	UFUNCTION(BlueprintPure, Category = "ARPG|VFX")
	FARPGStatusVfxStats GetStats() const;

	/** Hard ceiling on live visuals across every effect at once. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|VFX", meta = (ClampMin = "0"))
	int32 MaxInstances = 24;

	/** Afflictions further than this from the view get no visual, budget or not. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|VFX", meta = (ClampMin = "1.0"))
	float CullDistance = 8000.f;

	/**
	 * Seconds between passes. Spawning and freeing is not frame-critical, and
	 * re-sorting every frame would cost more than the visuals do.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|VFX", meta = (ClampMin = "0.05"))
	float ReevaluateInterval = 0.25f;

protected:
	/** Where "near" is measured from: the local view, or the origin without one. */
	FVector GetAnchorLocation() const;

	/** Spawns the visual and fits it to the target. */
	void Realise(FARPGStatusVfxRequest& Request, AActor* Target);

	/** Offers the visual a fade-out, and destroys it if it does not take one. */
	void Release(FARPGStatusVfxRequest& Request);

	/** Walks every registered ability system and marks what wants a visual. */
	void GatherRequests();

private:
	UPROPERTY(Transient)
	TMap<FARPGStatusVfxKey, FARPGStatusVfxRequest> Requests;

	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<UAbilitySystemComponent>> Registered;

	float Timer = 0.f;
};
