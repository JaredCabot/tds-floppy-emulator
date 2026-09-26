/**
 * clock.c - system clock: 144 MHz from the 8 MHz HEXT crystal via PLL.
 *
 * Verbatim from the Artery msc_only_fat32 example (correct for this board's
 * 8 MHz crystal). Kept as-is so the USB host phase inherits a known-good
 * 144 MHz / USB-48 MHz clock tree. See reference/msc_only_fat32.
 */
#include "clock.h"

void system_clock_config(void)
{
  crm_reset();
  flash_psr_set(FLASH_WAIT_CYCLE_4);

  crm_clock_source_enable(CRM_CLOCK_SOURCE_HEXT, TRUE);
  while(crm_hext_stable_wait() == ERROR) { }

  /* 8 MHz / 2 * 36 = 144 MHz */
  crm_pll_config(CRM_PLL_SOURCE_HEXT_DIV, CRM_PLL_MULT_36);
  crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);
  while(crm_flag_get(CRM_PLL_STABLE_FLAG) != SET) { }

  crm_ahb_div_set(CRM_AHB_DIV_1);     /* AHB  = 144 MHz */
  crm_apb2_div_set(CRM_APB2_DIV_2);   /* APB2 = 72 MHz  */
  crm_apb1_div_set(CRM_APB1_DIV_2);   /* APB1 = 72 MHz  */

  crm_auto_step_mode_enable(TRUE);
  crm_sysclk_switch(CRM_SCLK_PLL);
  while(crm_sysclk_switch_status_get() != CRM_SCLK_PLL) { }
  crm_auto_step_mode_enable(FALSE);

  system_core_clock_update();
}
