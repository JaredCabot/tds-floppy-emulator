/**
 * clock.c - system clock: 144 MHz from the 8 MHz HEXT crystal via PLL, as the
 * Artery msc_only_fat32 example (a known-good 144 MHz / USB 48 MHz tree).
 *
 * If the crystal does not start, the PLL runs from the internal oscillator
 * instead (RM_AT32F415 4.3.2: PLLRCS 0 = HICK / 12 = 4 MHz; x36 = the same
 * 144 MHz), rather than waiting for ever with the LED dark (review
 * 2026-09-28). HICK is factory-trimmed to +-1.5 % (DS Table 28): the floppy
 * stays within the drive's +-1.5 % rotation tolerance, USB (+-0.25 %) may
 * not work. clock_on_hick reports it (status report).
 */
#include "clock.h"

bool clock_on_hick;

void system_clock_config(void)
{
  crm_reset();
  flash_psr_set(FLASH_WAIT_CYCLE_4);

  crm_clock_source_enable(CRM_CLOCK_SOURCE_HEXT, TRUE);
  bool hext = false;                               /* one library wait is a few ms: allow */
  for(unsigned i = 0; i < 20 && !hext; i++)        /* a slow-starting crystal ~300 ms */
    hext = crm_hext_stable_wait() == SUCCESS;

  if(hext) crm_pll_config(CRM_PLL_SOURCE_HEXT_DIV, CRM_PLL_MULT_36);   /* 8 MHz / 2 x 36 */
  else
  {
    crm_clock_source_enable(CRM_CLOCK_SOURCE_HEXT, FALSE);
    crm_pll_config(CRM_PLL_SOURCE_HICK, CRM_PLL_MULT_36);              /* 4 MHz x 36 */
    clock_on_hick = true;
  }
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
