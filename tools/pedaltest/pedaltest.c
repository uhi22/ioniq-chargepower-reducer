/* Host test for src/pedalhandler.c: feeds a PCAN-View CSV trace into the pedal evaluation
   and prints every pedal press/release and every change of the current limit.

   Build (MinGW): gcc -Wall -Wextra -I../../src -o pedaltest.exe pedaltest.c ../../src/pedalhandler.c
   Run:           pedaltest.exe <trace.csv>   or   pedaltest.exe --selftest
   CSV format:    Time Stamp (us),ID (hex),Extended,Dir,Bus,LEN,D1,...,D8
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pedalhandler.h"

/* ---- synthetic self test: a charging session with pedal taps ---- */

static uint32_t simTime_ms = 0;
static int nFailed = 0;
static unsigned simPresentAmps = 0;   /* EVSE present current, sent by the "CCM" in 0x5E5 */
static bool simSendEvse = false;      /* whether 0x5E5 is sent */

/* runs the bus for duration_ms with the given pedal states: 0x200 every 10 ms, 0x394 every 20 ms,
   0x5E5 every 100 ms (if simSendEvse) */
static void simulate(uint32_t duration_ms, uint8_t accel, bool brake, bool sendPedals)
{
    uint32_t t;
    for (t = 0; t < duration_ms; t += 10) {
        uint8_t d200[8] = {0, 0xA8, 0x49, 0x04, accel, 0, 0x3C, 0};
        uint8_t d394[8] = {0, 0, 0, 0, 0, (uint8_t)(brake ? 0xC4 : 0x84), 0, 0};
        simTime_ms += 10;
        if (simSendEvse && (simTime_ms % 100) == 0) {
            unsigned dA = simPresentAmps * 10;
            uint8_t d5e5[8] = {0x94, 0x11, 0xD0, 0x07, 0x84, 0x0D, (uint8_t)(dA & 0xFF), (uint8_t)(dA >> 8)};
            pedalhandler_onCan2Frame(0x5E5, 8, d5e5, simTime_ms);
        }
        if (sendPedals) {
            pedalhandler_onCan1Frame(0x200, 8, d200, simTime_ms);
            if ((simTime_ms % 20) == 0) {
                d394[6] = brake ? 0x80 : 0x00;
                pedalhandler_onCan1Frame(0x394, 8, d394, simTime_ms);
            }
        }
        pedalhandler_mainfunction(simTime_ms);
    }
}

static void ccmProgress(uint8_t progress)
{
    uint8_t d5e9[8] = {progress, 0, 0, 0, 0, 0, 0, 0};
    pedalhandler_onCan2Frame(0x5E9, 8, d5e9, simTime_ms);
}

static void brakeTap(void) { simulate(300, 0, true, true); simulate(300, 0, false, true); }
static void accelTap(void) { simulate(300, 0x80, false, true); simulate(300, 0, false, true); }

static void expect(const char *step, unsigned limitAmps)
{
    unsigned is = pedal_currentLimitDeciAmps / 10;
    printf("%-66s limit %3u A  %s\n", step, is, is == limitAmps ? "ok" : "FAILED");
    if (is != limitAmps) nFailed++;
}

static int selftest(void)
{
    int i;
    simulate(1000, 0, false, true);
    expect("idle, nothing pressed", 100);
    for (i = 0; i < 3; i++) brakeTap();
    expect("3 brake taps before the session: ignored", 100);
    ccmProgress(0x00); ccmProgress(0x03);
    expect("session starts (CCM progress 0 -> 2): default", 100);
    ccmProgress(0x0F); simSendEvse = true; simPresentAmps = 0;
    brakeTap();
    expect("brake tap during PreCharge: ignored", 100);

    ccmProgress(0x3F); simPresentAmps = 100;
    brakeTap();
    expect("CurrentDemand, delivered 100 A (our limit), brake tap", 90);
    simPresentAmps = 175;
    brakeTap();
    expect("delivered 175 A (car is the bottleneck), brake tap: visible", 165);
    simPresentAmps = 150;
    brakeTap();
    expect("delivered 150 A (small charger), brake tap: visible", 140);
    simPresentAmps = 140;
    accelTap();
    expect("delivered 140 A, accelerator tap", 150);
    simulate(2000, 0, true, true); simulate(300, 0, false, true);
    expect("long brake press (2 s): ignored", 150);
    simulate(300, 0x30, false, true); simulate(300, 0, false, true);
    expect("accelerator below threshold (19 %): ignored", 150);
    simPresentAmps = 150;
    simulate(300, 0x80, false, true); simulate(300, 0x20, false, true); simulate(300, 0, false, true);
    expect("accelerator tap with slow release (hysteresis)", 160);
    simPresentAmps = 195;
    accelTap();
    expect("delivered 195 A, accelerator tap: clamped at max", 200);
    simPresentAmps = 35;
    brakeTap();
    expect("delivered 35 A, brake tap: clamped at min", 30);
    simPresentAmps = 100;
    simulate(300, 0, true, true); simulate(1000, 0, true, false); simulate(300, 0, false, true);
    expect("brake pressed, then pedal signal lost, then released: ignored", 30);
    simSendEvse = false; simulate(1000, 0, false, true);
    brakeTap();
    expect("EVSE present current missing (0x5E5 timeout): tap ignored", 30);
    simSendEvse = true;
    ccmProgress(0x3F);
    expect("session continues (progress 0x3E): no reset", 30);
    ccmProgress(0x47); ccmProgress(0x01); ccmProgress(0x03);
    expect("session stop, idle, new session: reset", 100);
    printf("taps: accel %u, brake %u, ignored %u\n", (unsigned)nAccelTaps, (unsigned)nBrakeTaps, (unsigned)nIgnoredTaps);
    printf("%s\n", nFailed ? "SELFTEST FAILED" : "selftest passed");
    return nFailed ? 1 : 0;
}

int main(int argc, char *argv[])
{
    FILE *f;
    char line[512];
    unsigned long tsPrev = 0;
    unsigned long long now_us = 0;
    uint32_t now_ms = 0;
    bool accelPrev = false, brakePrev = false;
    uint32_t tAccel = 0, tBrake = 0;
    uint16_t limitPrev = pedal_currentLimitDeciAmps;

    if (argc < 2) {
        printf("usage: pedaltest <trace.csv> | --selftest\n");
        return 1;
    }
    if (strcmp(argv[1], "--selftest") == 0) {
        return selftest();
    }
    f = fopen(argv[1], "r");
    if (!f) {
        printf("cannot open %s\n", argv[1]);
        return 1;
    }
    printf("start: limit %u A\n", pedal_currentLimitDeciAmps / 10);
    while (fgets(line, sizeof(line), f)) {
        unsigned long ts;
        unsigned int id, len, d[8] = {0};
        uint8_t data[8];
        int i;
        if (sscanf(line, "%lu,%x,%*[^,],%*[^,],%*[^,],%u,%x,%x,%x,%x,%x,%x,%x,%x",
                   &ts, &id, &len, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) < 3) {
            continue; /* header */
        }
        /* the trace time stamp jumps backwards once (logger restart): continue the time monotonic */
        if (tsPrev != 0 && ts >= tsPrev) now_us += ts - tsPrev;
        tsPrev = ts;
        now_ms = (uint32_t)(now_us / 1000);
        for (i = 0; i < 8; i++) data[i] = (uint8_t)d[i];

        pedalhandler_onCan1Frame(id, (uint8_t)len, data, now_ms);
        pedalhandler_mainfunction(now_ms);

        if (pedal_accelPressed != accelPrev) {
            if (pedal_accelPressed) tAccel = now_ms;
            else printf("%8.3f s  accel released after %5u ms\n", now_ms / 1000.0, now_ms - tAccel);
            accelPrev = pedal_accelPressed;
        }
        if (pedal_brakePressed != brakePrev) {
            if (pedal_brakePressed) tBrake = now_ms;
            else printf("%8.3f s  brake released after %5u ms\n", now_ms / 1000.0, now_ms - tBrake);
            brakePrev = pedal_brakePressed;
        }
        if (pedal_currentLimitDeciAmps != limitPrev) {
            printf("%8.3f s  -> limit %u A  (taps accel %u, brake %u)\n", now_ms / 1000.0,
                   pedal_currentLimitDeciAmps / 10, (unsigned)nAccelTaps, (unsigned)nBrakeTaps);
            limitPrev = pedal_currentLimitDeciAmps;
        }
    }
    fclose(f);
    printf("end: limit %u A, taps accel %u, brake %u\n", pedal_currentLimitDeciAmps / 10,
           (unsigned)nAccelTaps, (unsigned)nBrakeTaps);
    return 0;
}
