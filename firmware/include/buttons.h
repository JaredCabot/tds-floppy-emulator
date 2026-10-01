#ifndef BUTTONS_H
#define BUTTONS_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Front-panel button gestures (pure logic, host-tested; main.c feeds it the pin
 * state every 1 ms and acts on the result).
 *
 *   one button pressed and released on its own  -> BUTTON_OUT / BUTTON_IN
 *   both held together for BUTTONS_HOLD_MS       -> armed (red LED solid), then
 *                                                   on full release BUTTON_UPDATE
 *   both pressed, but not long enough            -> nothing
 *
 * Actions fire on RELEASE, so starting a two-button hold can never trigger
 * DATA IN / DATA OUT first. Contacts are debounced (BUTTONS_DEBOUNCE_MS stable).
 */
#define BUTTON_OUT          0x01u
#define BUTTON_IN         0x02u
#define BUTTON_UPDATE        0x100u
#define BUTTONS_DEBOUNCE_MS  30u
#define BUTTONS_HOLD_MS      3000u

typedef struct {
  uint16_t stable, last, seen;
  uint32_t same_ms, both_ms;
  bool     armed;
} buttons_t;

/* raw: BUTTON_OUT / BUTTON_IN bits currently pressed. Call every 1 ms.
 * Returns a gesture (BUTTON_OUT, BUTTON_IN, BUTTON_UPDATE) or 0. */
uint16_t buttons_step(buttons_t *b, uint16_t raw);

/* After a pause in the 1 ms calls (a transfer blocks the main loop for seconds),
 * start again from the pins as they are now, instead of replaying the pause as
 * one long press: whatever was in progress is dropped, and a button held across
 * the pause gives no gesture when released. (Holding both for BUTTONS_HOLD_MS
 * after the pause is a new, deliberate update gesture.) */
#define BUTTONS_STALE        0x80u
void buttons_resync(buttons_t *b, uint16_t raw);

/* Both buttons have been held long enough: an update will start on release. */
static inline bool buttons_armed(const buttons_t *b) { return b->armed; }

#endif /* BUTTONS_H */
