#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/can.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/cm3/dwt.h>
#include <libopencm3/cm3/scs.h>
#include <libopencm3/stm32/iwdg.h>
#include <libopencm3/stm32/dbgmcu.h>
#include <stdint.h>
#include <stdbool.h>
#include "serialhandler.h"
#include "canhandler.h"
#include "pedalhandler.h"

static volatile uint32_t tick_ms = 0;

/* 25 MHz HSE → PLL2 chain → 72 MHz SYSCLK, APB1 = 36 MHz */
static void clock_setup(void)
{
    rcc_clock_setup_pll(&rcc_hse_configs[RCC_CLOCK_HSE25_72MHZ]);
}

void sys_tick_handler(void)
{
    tick_ms++;
}

static void dwt_setup(void)
{
    SCS_DEMCR  |= SCS_DEMCR_TRCENA;   /* enable trace */
    DWT_CYCCNT  = 0;
    DWT_CTRL   |= DWT_CTRL_CYCCNTENA; /* start cycle counter */
}

static void systick_setup(void)
{
    /* 72 MHz / 72000 = 1 kHz → 1 ms tick */
    systick_set_reload(72000 - 1);
    systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
    systick_counter_enable();
    systick_interrupt_enable();
}

/* Independent watchdog on LSI (~40 kHz). Freezes while the core is halted by the debugger. */
static void iwdg_setup(void)
{
    DBGMCU_CR |= DBGMCU_CR_IWDG_STOP;
    iwdg_set_period_ms(100);
    iwdg_start();
}

static void task_5ms(void)
{
    static uint16_t taskdivider;
    taskdivider++;
    /* just for bring-up: send some demo messages to both CAN busses */
    //if ((taskdivider%16)==0) canhandler_sendDemoMessage_CAN1();
    //if ((taskdivider%16)==8) canhandler_sendDemoMessage_CAN2();
}

static void task_1000ms(void)
{
    uint32_t uptime_s = tick_ms / 1000;
    serialhandler_puts("uptime: ");
    serialhandler_print_uint32(uptime_s);
    serialhandler_puts("s  txdrop: ");
    serialhandler_print_uint32(nSerialTxDrop);
    serialhandler_puts("\r\n");

    serialhandler_puts("CAN1 rx: ");
    serialhandler_print_uint32(nCan1TotalReceived);
    serialhandler_puts("  dropped: ");
    serialhandler_print_uint32(nCan1Dropped);
    serialhandler_puts("  rx/s: ");
    serialhandler_print_uint32(nCan1ReceivedFramesPerSecond);
    serialhandler_puts("  drop/s: ");
    serialhandler_print_uint32(nCan1DroppedFramesPerSecond);
    serialhandler_puts("  txdrop: ");
    serialhandler_print_uint32(nCan1TxDropped);
    serialhandler_puts("\r\n");

    serialhandler_puts("CAN2 rx: ");
    serialhandler_print_uint32(nCan2TotalReceived);
    serialhandler_puts("  dropped: ");
    serialhandler_print_uint32(nCan2Dropped);
    serialhandler_puts("  rx/s: ");
    serialhandler_print_uint32(nCan2ReceivedFramesPerSecond);
    serialhandler_puts("  drop/s: ");
    serialhandler_print_uint32(nCan2DroppedFramesPerSecond);
    serialhandler_puts("  txdrop: ");
    serialhandler_print_uint32(nCan2TxDropped);
    serialhandler_puts("\r\n");

    serialhandler_puts("limit: ");
    serialhandler_print_uint32(pedal_currentLimitDeciAmps / 10);
    serialhandler_puts("A  present: ");
    serialhandler_print_uint32(pedal_presentCurrentDeciAmps / 10);
    serialhandler_puts("A  limited: ");
    serialhandler_print_uint32(nTargetCurrentLimited);
    serialhandler_puts("  accel: ");
    serialhandler_print_uint32(pedal_accelRaw);
    serialhandler_puts(pedal_accelValid ? (pedal_accelPressed ? " pressed" : " released") : " missing");
    serialhandler_puts(" taps ");
    serialhandler_print_uint32(nAccelTaps);
    serialhandler_puts("  brake:");
    serialhandler_puts(pedal_brakeValid ? (pedal_brakePressed ? " pressed" : " released") : " missing");
    serialhandler_puts(" taps ");
    serialhandler_print_uint32(nBrakeTaps);
    serialhandler_puts("  ignored taps: ");
    serialhandler_print_uint32(nIgnoredTaps);
    serialhandler_puts("  sessions: ");
    serialhandler_print_uint32(nSessionStarts);
    serialhandler_puts("\r\n");

    nCan1ReceivedFramesPerSecond = 0;
    nCan1DroppedFramesPerSecond  = 0;
    nCan2ReceivedFramesPerSecond = 0;
    nCan2DroppedFramesPerSecond  = 0;

    for (int i = 0; i < CAN1_MSG_TABLE_SIZE; i++) {
        if (!can1_msg_table[i].valid)
            continue;
        serialhandler_puts("  CAN1[");
        serialhandler_print_uint32(i);
        serialhandler_puts("] id=0x");
        serialhandler_print_hex32(can1_msg_table[i].id);
        serialhandler_puts(" dlc=");
        serialhandler_print_uint32(can1_msg_table[i].dlc);
        serialhandler_puts(" data=");
        for (uint8_t b = 0; b < can1_msg_table[i].dlc; b++) {
            serialhandler_print_hex8(can1_msg_table[i].data[b]);
            if (b + 1 < can1_msg_table[i].dlc)
                serialhandler_puts(" ");
        }
        serialhandler_puts(" cnt=");
        serialhandler_print_uint32(can1_msg_table[i].rx_count);
        serialhandler_puts(" t=");
        serialhandler_print_uint32(can1_msg_table[i].timestamp_ms);
        serialhandler_puts("ms\r\n");
    }

    for (int i = 0; i < CAN2_MSG_TABLE_SIZE; i++) {
        if (!can2_msg_table[i].valid)
            continue;
        serialhandler_puts("  CAN2[");
        serialhandler_print_uint32(i);
        serialhandler_puts("] id=0x");
        serialhandler_print_hex32(can2_msg_table[i].id);
        serialhandler_puts(" dlc=");
        serialhandler_print_uint32(can2_msg_table[i].dlc);
        serialhandler_puts(" data=");
        for (uint8_t b = 0; b < can2_msg_table[i].dlc; b++) {
            serialhandler_print_hex8(can2_msg_table[i].data[b]);
            if (b + 1 < can2_msg_table[i].dlc)
                serialhandler_puts(" ");
        }
        serialhandler_puts(" cnt=");
        serialhandler_print_uint32(can2_msg_table[i].rx_count);
        serialhandler_puts(" t=");
        serialhandler_print_uint32(can2_msg_table[i].timestamp_ms);
        serialhandler_puts("ms\r\n");
    }
}

int main(void)
{
    clock_setup();
    dwt_setup();
    serialhandler_setup();
    canhandler_setup();
    systick_setup();
    iwdg_setup();

    serialhandler_puts("Hello World. ioniq-chargepower-reducer, build " __DATE__ " " __TIME__ "\r\n");

    uint32_t last_5ms    = 0;
    uint32_t last_1000ms = 0;

    while (1) {
        uint32_t now = tick_ms;

        iwdg_reset();

        canhandler_process_rx1(now);
        canhandler_process_rx2(now);
        pedalhandler_mainfunction(now);

        if ((now - last_5ms) >= 5) {
            last_5ms = now;
            task_5ms();
        }

        if ((now - last_1000ms) >= 1000) {
            last_1000ms = now;
            /* duration of the serial output, printed for monitoring the main loop blocking time.
               Only the messages of interest are in the tables (see canhandler.c, loggedIds). */
            uint32_t t0 = DWT_CYCCNT;
            task_1000ms();
            serialhandler_puts("task_1000ms: ");
            serialhandler_print_uint32((DWT_CYCCNT - t0) / 72);
            serialhandler_puts("us\r\n");
        }
    }

    return 0;
}
