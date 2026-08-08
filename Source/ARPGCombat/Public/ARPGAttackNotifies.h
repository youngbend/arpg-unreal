// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "ARPGAttackNotifies.generated.h"

/**
 * The attack phase boundaries, as anim notifies.
 *
 * These replace the elapsed-time bookkeeping animation.gd did by hand. Godot
 * stored clip NAMES and had to track how far through each phase it was in order
 * to know when to arm the hitbox or open the cancel window; a montage already
 * knows where it is, so the boundary just becomes a marker on the timeline.
 *
 * Both notifies only RAISE A GAMEPLAY EVENT. They deliberately do not touch the
 * hitbox or the combo component directly: an anim notify fires on clients as
 * well as the server, and hit detection is server-authoritative. Routing through
 * the ability system means the listener decides what authority it needs, and the
 * same montage stays valid whether it is played by a player, an NPC, or a
 * replay.
 */

/**
 * Spans the active window of a swing. Begin arms the hitbox, End disarms it.
 *
 * A notify STATE rather than two notifies because a swing's active window is one
 * thing with a duration -- scrubbing or retiming it in the montage editor keeps
 * both ends together, and a montage that is interrupted mid-swing still gets its
 * End call, so the hitbox cannot be left armed.
 */
UCLASS(meta = (DisplayName = "ARPG Hitbox Window"))
class ARPGCOMBAT_API UARPGAnimNotifyState_Hitbox : public UAnimNotifyState
{
	GENERATED_BODY()

public:
	UARPGAnimNotifyState_Hitbox();

	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
		float TotalDuration, const FAnimNotifyEventReference& EventReference) override;

	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
		const FAnimNotifyEventReference& EventReference) override;

#if WITH_EDITOR
	virtual FString GetNotifyName_Implementation() const override;
#endif

	/**
	 * Which active window this is. 0 for the first, 1 for a second active window
	 * on a double-hit attack -- the ability uses it to pick MotionValue versus
	 * MotionValue2, so a two-part swing can hit for different amounts.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG", meta = (ClampMin = "0"))
	int32 WindowIndex = 0;

	/**
	 * Marks this window as a landing impact rather than a swing, so the ability
	 * uses LandingMotionValue. An air attack's landing hit is authored as its own
	 * window on the same montage.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ARPG")
	bool bIsLandingWindow = false;
};

/**
 * Opens the combo cancel window. Placed inside Recovery at the attack's authored
 * RecoveryCancelDelay -- the value the .tres converter carried across precisely
 * so this timing did not have to be re-guessed.
 *
 * From here the combo component will fire a buffered press, auto-chain a
 * continuous-hold attack, or start the reset timer.
 */
UCLASS(meta = (DisplayName = "ARPG Combo Window"))
class ARPGCOMBAT_API UARPGAnimNotify_ComboWindow : public UAnimNotify
{
	GENERATED_BODY()

public:
	UARPGAnimNotify_ComboWindow();

	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
		const FAnimNotifyEventReference& EventReference) override;

#if WITH_EDITOR
	virtual FString GetNotifyName_Implementation() const override;
#endif
};
