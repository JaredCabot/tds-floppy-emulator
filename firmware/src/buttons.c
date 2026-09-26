/*
 * buttons.c - debounce + gesture logic for the two front-panel buttons
 * (see buttons.h). Pure C: no hardware access.
 */
#include "buttons.h"

uint16_t buttons_step(buttons_t *b, uint16_t raw)
{
  raw &= BUTTON_LEFT | BUTTON_RIGHT;

  /* debounce: accept a new state once it has been stable for BUTTONS_DEBOUNCE_MS */
  if(raw != b->last) { b->last = raw; b->same_ms = 0; }
  else if(b->same_ms < BUTTONS_DEBOUNCE_MS && ++b->same_ms == BUTTONS_DEBOUNCE_MS) b->stable = raw;

  uint16_t s = b->stable;
  if(s)                                   /* gesture in progress */
  {
    b->seen |= s;
    if(s == (BUTTON_LEFT | BUTTON_RIGHT))
    {
      if(!b->armed && ++b->both_ms >= BUTTONS_HOLD_MS) b->armed = true;
    }
    else b->both_ms = 0;                  /* both must be held continuously */
    return 0;
  }
  if(!b->seen) return 0;                  /* idle */

  /* all released: decide */
  uint16_t g = b->armed ? BUTTON_UPDATE     /* (a STALE press never matches: nothing) */
             : (b->seen == BUTTON_LEFT || b->seen == BUTTON_RIGHT) ? b->seen : 0;
  b->seen = 0; b->both_ms = 0; b->armed = false;
  return g;
}

void buttons_resync(buttons_t *b, uint16_t raw)
{
  raw &= BUTTON_LEFT | BUTTON_RIGHT;
  b->last = b->stable = raw;               /* take the pins as they are now */
  b->same_ms = BUTTONS_DEBOUNCE_MS;
  b->both_ms = 0;
  b->armed = false;
  b->seen = raw ? BUTTONS_STALE : 0;       /* held across the pause: no gesture on release */
}
