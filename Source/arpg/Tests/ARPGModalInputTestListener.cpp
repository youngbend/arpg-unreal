// Copyright Epic Games, Inc. All Rights Reserved.

#include "ARPGModalInputTestListener.h"

void UARPGModalInputTestListener::Bind(UARPGModalInputComponent* Input)
{
	if (!Input)
	{
		return;
	}

	Input->OnLightAttack.AddDynamic(this, &UARPGModalInputTestListener::OnLightAttack);
	Input->OnLightAttackReleased.AddDynamic(this, &UARPGModalInputTestListener::OnLightAttackReleased);
	Input->OnHeavyAttack.AddDynamic(this, &UARPGModalInputTestListener::OnHeavyAttack);
	Input->OnHeavyAttackReleased.AddDynamic(this, &UARPGModalInputTestListener::OnHeavyAttackReleased);
	Input->OnSpecialAttack.AddDynamic(this, &UARPGModalInputTestListener::OnSpecialAttack);
	Input->OnSpecialAttackReleased.AddDynamic(this, &UARPGModalInputTestListener::OnSpecialAttackReleased);
	Input->OnJump.AddDynamic(this, &UARPGModalInputTestListener::OnJump);
	Input->OnDodge.AddDynamic(this, &UARPGModalInputTestListener::OnDodge);
	Input->OnParryPressed.AddDynamic(this, &UARPGModalInputTestListener::OnParryPressed);
	Input->OnParryReleased.AddDynamic(this, &UARPGModalInputTestListener::OnParryReleased);
	Input->OnSheathePressed.AddDynamic(this, &UARPGModalInputTestListener::OnSheathe);
	Input->OnMagicSelect.AddDynamic(this, &UARPGModalInputTestListener::OnMagicSelect);
	Input->OnMagicDiscard.AddDynamic(this, &UARPGModalInputTestListener::OnMagicDiscard);
	Input->OnMagicPagePrev.AddDynamic(this, &UARPGModalInputTestListener::OnMagicPagePrev);
	Input->OnMagicPageNext.AddDynamic(this, &UARPGModalInputTestListener::OnMagicPageNext);
	Input->OnQuickSlotPrev.AddDynamic(this, &UARPGModalInputTestListener::OnQuickSlotPrev);
	Input->OnQuickSlotNext.AddDynamic(this, &UARPGModalInputTestListener::OnQuickSlotNext);
	Input->OnQuickSlotUse.AddDynamic(this, &UARPGModalInputTestListener::OnQuickSlotUse);
	Input->OnDischargeModifierPressed.AddDynamic(this,
		&UARPGModalInputTestListener::OnDischargeModifierPressed);
	Input->OnDischargeChargeStarted.AddDynamic(this,
		&UARPGModalInputTestListener::OnDischargeChargeStarted);
	Input->OnDischargeActivated.AddDynamic(this, &UARPGModalInputTestListener::OnDischargeActivated);
	Input->OnDischargeCancelled.AddDynamic(this, &UARPGModalInputTestListener::OnDischargeCancelled);
}
