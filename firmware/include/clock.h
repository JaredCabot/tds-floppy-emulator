#ifndef CLOCK_H
#define CLOCK_H
#include "at32f415_conf.h"
#include <stdbool.h>
/* Configure the system clock to 144 MHz from the 8 MHz HEXT crystal, or from
 * the internal oscillator if the crystal does not start (clock_on_hick). */
void system_clock_config(void);
extern bool clock_on_hick;
#endif
