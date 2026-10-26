#ifndef PEDALHANDLER_H
#define PEDALHANDLER_H

#include <stdint.h>
#include <stdbool.h>

/* Charge current limit, adjusted by tapping the pedals (brake = lower, accelerator = higher).
   Unit 0.1 A, same scaling as the target current in 0x59C B4..B5. */
extern uint16_t pedal_currentLimitDeciAmps;

extern uint8_t  pedal_accelRaw;        /* last accelerator pedal value, 0..255 */
extern bool     pedal_accelPressed;
extern bool     pedal_brakePressed;
extern bool     pedal_accelValid;      /* false if 0x200 is missing (timeout) */
extern bool     pedal_brakeValid;      /* false if 0x394 is missing (timeout) */
extern uint32_t nAccelTaps;
extern uint32_t nBrakeTaps;
extern uint32_t nSessionStarts;
extern uint32_t nIgnoredTaps;          /* taps outside of CurrentDemand, counted but without effect */
extern uint16_t pedal_presentCurrentDeciAmps;  /* EVSE present current from the CCM (0x5E5), 0.1 A */

void pedalhandler_onCan1Frame(uint32_t id, uint8_t dlc, const uint8_t *data, uint32_t now_ms);
void pedalhandler_onCan2Frame(uint32_t id, uint8_t dlc, const uint8_t *data, uint32_t now_ms);
void pedalhandler_mainfunction(uint32_t now_ms);

#endif /* PEDALHANDLER_H */
