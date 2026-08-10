// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ARPGHandVisualComponent.generated.h"

class UARPGMagicComponent;
class UARPGMagicElement;

/**
 * Shows what is readied, in the caster's hand.
 *
 * Small, but it closes the one gap that makes the whole magic scheme hard to
 * believe while testing: LT readies an element, and until something appears
 * there is no way to tell whether it worked. Everything downstream -- that a
 * swing imbues, that a dodge goes elemental, that RT discharges what is in
 * hand -- is invisible without it.
 *
 * Tracks the DISPLAY element rather than the raw selection, so readying fire
 * and then water shows the steam that resolves from them rather than two
 * separate motes. That is what the player is actually holding: a combination is
 * one thing, not two.
 *
 * Purely cosmetic and purely local -- it runs off replicated selection state, so
 * every machine builds its own and nothing is sent for it.
 */
UCLASS(ClassGroup = (ARPG), meta = (BlueprintSpawnableComponent))
class ARPGMAGIC_API UARPGHandVisualComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UARPGHandVisualComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * Socket on the owner's skeletal mesh to hang the visual from.
	 *
	 * Falls back to the actor itself when the socket does not exist, so a
	 * character on a skeleton that has not been set up yet still shows
	 * something -- badly placed beats absent when the question is "did my input
	 * register".
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ARPG|Magic")
	FName HandSocket = TEXT("hand_l");

	UFUNCTION(BlueprintPure, Category = "ARPG|Magic")
	AActor* GetHandEffect() const { return HandEffect; }

	/** Spawns, swaps or clears the visual to match what is readied now. */
	UFUNCTION(BlueprintCallable, Category = "ARPG|Magic")
	void Refresh();

protected:
	UFUNCTION()
	void HandleSelectionChanged(int32 ActiveMask);

	UARPGMagicComponent* GetMagic() const;

	void Clear();

private:
	UPROPERTY(Transient)
	TObjectPtr<AActor> HandEffect;

	/**
	 * What the current visual is for, so an unchanged selection does not respawn
	 * it every time the mask is touched.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UARPGMagicElement> ShownElement;

	UPROPERTY(Transient)
	TObjectPtr<UARPGMagicComponent> BoundMagic;
};
