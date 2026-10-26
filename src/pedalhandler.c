/* Pedal evaluation: the driver adjusts the charge current limit by tapping the pedals.
   - brake tap       -> limit = delivered current - one step
   - accelerator tap -> limit = delivered current + one step
   The base is the current which the charger actually delivers (EVSE present current,
   reported by the CCM in 0x5E5), because this is what the driver sees on the power display,
   independent of whether the car, the charger or our limit is the bottleneck. So every
   brake tap has a visible effect, and taps do not accumulate invisibly.
   Taps are evaluated only during CurrentDemand (before, no current flows and the base is
   unknown). Other taps are counted, but ignored.
   A tap is a press which is released again within TAP_MAX_DURATION_MS. Longer presses
   are ignored, so that normal driving or holding a pedal does not change the limit.
   At the start of each charging session (CCM progress leaves idle), the limit is set
   back to the default.

   Signals (verified with the drive trace IONIQ_PCAN_drive_fwd_neutral_drive_reverse_neutral.csv):
   - 0x200 (VCU200) B4: accelerator pedal, 0..255 (B7 is the same in %), 10 ms
   - 0x394 (TCS13) B6 bit 7: DriverBraking, 1 = brake pressed, 20 ms
   Not yet verified: whether both signals also react while the car is charging (not READY).
*/
#include <stdint.h>
#include <stdbool.h>
#include "pedalhandler.h"

#define CAN_ID_ACCEL                  0x200
#define CAN_ID_BRAKE                  0x394
#define CAN_ID_CCM_PROGRESS           0x5E9  /* B0 without bit 0: 0 = idle, 0x02...0x3E session running */
#define CAN_ID_CCM_EVSE_VALUES        0x5E5  /* B6..B7 EVSE present current, LE16, 0.1 A */
#define CCM_PROGRESS_CURRENTDEMAND    0x3E

#define ACCEL_PRESSED_THRESHOLD       0x40   /* 25 % */
#define ACCEL_RELEASED_THRESHOLD      0x10   /* 6 %, hysteresis */
#define TAP_MAX_DURATION_MS           1000
#define SIGNAL_TIMEOUT_MS             500

#define CURRENT_LIMIT_DEFAULT_DECIAMPS  1000 /* 100 A */
#define CURRENT_LIMIT_STEP_DECIAMPS      100 /*  10 A */
#define CURRENT_LIMIT_MIN_DECIAMPS       300 /*  30 A */
#define CURRENT_LIMIT_MAX_DECIAMPS      2000 /* 200 A, the EVMaximumCurrentLimit of the car, so no limit */

typedef struct {
    bool     pressed;
    uint32_t tPressed_ms;
    uint32_t tLastRx_ms;
    bool     valid;
} pedal_t;

static pedal_t accel;
static pedal_t brake;
static uint8_t lastCcmProgress = 0;
static uint32_t tLastEvseValues_ms;
static bool     evseValuesReceived = false;

uint16_t pedal_currentLimitDeciAmps = CURRENT_LIMIT_DEFAULT_DECIAMPS;
uint8_t  pedal_accelRaw = 0;
bool     pedal_accelPressed = false;
bool     pedal_brakePressed = false;
bool     pedal_accelValid = false;
bool     pedal_brakeValid = false;
uint32_t nAccelTaps = 0;
uint32_t nBrakeTaps = 0;
uint32_t nSessionStarts = 0;
uint32_t nIgnoredTaps = 0;
uint16_t pedal_presentCurrentDeciAmps = 0;

/* A tap sets the limit relative to the delivered current. Only during CurrentDemand,
   with a recent EVSE present current; otherwise the tap is ignored. */
static void changeLimit(int16_t deltaDeciAmps, uint32_t now_ms)
{
    int32_t limit;
    if ((lastCcmProgress != CCM_PROGRESS_CURRENTDEMAND) || !evseValuesReceived
            || ((now_ms - tLastEvseValues_ms) > SIGNAL_TIMEOUT_MS)) {
        nIgnoredTaps++;
        return;
    }
    limit = (int32_t)pedal_presentCurrentDeciAmps + deltaDeciAmps;
    if (limit < CURRENT_LIMIT_MIN_DECIAMPS) limit = CURRENT_LIMIT_MIN_DECIAMPS;
    if (limit > CURRENT_LIMIT_MAX_DECIAMPS) limit = CURRENT_LIMIT_MAX_DECIAMPS;
    pedal_currentLimitDeciAmps = (uint16_t)limit;
}

/* returns true if a tap (short press) was just completed */
static bool updatePedal(pedal_t *p, bool pressedNow, uint32_t now_ms)
{
    bool tap = false;
    p->tLastRx_ms = now_ms;
    p->valid = true;
    if (pressedNow && !p->pressed) {
        p->pressed = true;
        p->tPressed_ms = now_ms;
    } else if (!pressedNow && p->pressed) {
        p->pressed = false;
        tap = (now_ms - p->tPressed_ms) <= TAP_MAX_DURATION_MS;
    }
    return tap;
}

void pedalhandler_onCan1Frame(uint32_t id, uint8_t dlc, const uint8_t *data, uint32_t now_ms)
{
    if ((id == CAN_ID_ACCEL) && (dlc >= 5)) {
        bool pressedNow;
        pedal_accelRaw = data[4];
        if (accel.pressed) {
            pressedNow = pedal_accelRaw >= ACCEL_RELEASED_THRESHOLD;
        } else {
            pressedNow = pedal_accelRaw >= ACCEL_PRESSED_THRESHOLD;
        }
        if (updatePedal(&accel, pressedNow, now_ms)) {
            nAccelTaps++;
            changeLimit(CURRENT_LIMIT_STEP_DECIAMPS, now_ms);
        }
        pedal_accelPressed = accel.pressed;
        pedal_accelValid = accel.valid;
    }
    if ((id == CAN_ID_BRAKE) && (dlc >= 7)) {
        if (updatePedal(&brake, (data[6] & 0x80) != 0, now_ms)) {
            nBrakeTaps++;
            changeLimit(-CURRENT_LIMIT_STEP_DECIAMPS, now_ms);
        }
        pedal_brakePressed = brake.pressed;
        pedal_brakeValid = brake.valid;
    }
}

void pedalhandler_onCan2Frame(uint32_t id, uint8_t dlc, const uint8_t *data, uint32_t now_ms)
{
    if ((id == CAN_ID_CCM_EVSE_VALUES) && (dlc >= 8)) {
        pedal_presentCurrentDeciAmps = data[6] | (data[7] << 8);
        tLastEvseValues_ms = now_ms;
        evseValuesReceived = true;
    }
    if ((id == CAN_ID_CCM_PROGRESS) && (dlc >= 1)) {
        uint8_t progress = data[0] & 0xFE; /* bit 0 is an alive toggle */
        if ((lastCcmProgress == 0) && (progress != 0)) {
            /* a new charging session starts */
            nSessionStarts++;
            pedal_currentLimitDeciAmps = CURRENT_LIMIT_DEFAULT_DECIAMPS;
        }
        lastCcmProgress = progress;
    }
}

static void checkTimeout(pedal_t *p, uint32_t now_ms)
{
    if (p->valid && ((now_ms - p->tLastRx_ms) > SIGNAL_TIMEOUT_MS)) {
        /* signal lost: forget a running press, it shall not count as a tap */
        p->valid = false;
        p->pressed = false;
    }
}

void pedalhandler_mainfunction(uint32_t now_ms)
{
    checkTimeout(&accel, now_ms);
    checkTimeout(&brake, now_ms);
    pedal_accelPressed = accel.pressed;
    pedal_accelValid = accel.valid;
    pedal_brakePressed = brake.pressed;
    pedal_brakeValid = brake.valid;
}
