#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/can.h>
#include <libopencm3/cm3/nvic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "canhandler.h"

#define CAN1_RX_RING_SIZE 32   /* must be power of 2 */
#define CAN2_RX_RING_SIZE 32

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} can1_ring_entry_t;

static volatile can1_ring_entry_t can1_rx_ring[CAN1_RX_RING_SIZE];
static volatile uint8_t           can1_rx_ring_head = 0;
static volatile uint8_t           can1_rx_ring_tail = 0;

static volatile can1_ring_entry_t can2_rx_ring[CAN2_RX_RING_SIZE];
static volatile uint8_t           can2_rx_ring_head = 0;
static volatile uint8_t           can2_rx_ring_tail = 0;

can1_msg_entry_t  can1_msg_table[CAN1_MSG_TABLE_SIZE];
uint32_t          nCan1TotalReceived = 0;
volatile uint32_t nCan1Dropped = 0;
volatile uint32_t nCan1ReceivedFramesPerSecond = 0;
volatile uint32_t nCan1DroppedFramesPerSecond = 0;

can1_msg_entry_t  can2_msg_table[CAN2_MSG_TABLE_SIZE];
uint32_t          nCan2TotalReceived = 0;
volatile uint32_t nCan2Dropped = 0;
volatile uint32_t nCan2ReceivedFramesPerSecond = 0;
volatile uint32_t nCan2DroppedFramesPerSecond = 0;


uint32_t can1_rxid;
uint8_t  can1_rxdlc;
uint8_t  can1_rxdata[8];
uint8_t  can1_txdata[8];

uint32_t can2_rxid;
uint8_t  can2_rxdlc;
uint8_t  can2_rxdata[8];
uint8_t  can2_txdata[8];

void canhandler_routeFromCan1ToCan2(void) {
    uint8_t i;
    for (i=0; i<8; i++) can2_txdata[i] = can1_rxdata[i]; /* copy originally first */
    can2_txdata[0] ^= 0xff; /* do modification */
    can_transmit(CAN2,
                 can1_rxid,   /* CAN ID */
                 false,   /* extended ID */
                 false,   /* RTR */
                 can1_rxdlc,       /* DLC */
                 can2_txdata);
}

void canhandler_routeFromCan2ToCan1(void) {
    uint8_t i;
    for (i=0; i<8; i++) can1_txdata[i] = can2_rxdata[i]; /* copy originally first */
    can1_txdata[0] = 0x11; /* do modification */
    can_transmit(CAN1,
                 can2_rxid,   /* CAN ID */
                 false,   /* extended ID */
                 false,   /* RTR */
                 can2_rxdlc,       /* DLC */
                 can1_txdata);
}


void canhandler_sendDemoMessage_CAN1(void) {
    static uint16_t msg_counter1 = 0;
    uint8_t data[8] = {
        (uint8_t)(msg_counter1 >> 8),
        (uint8_t)(msg_counter1 & 0xFF),
        0x33, 0x44, 0x55, 0x66, 0x77, 0x88
    };
    can_transmit(CAN1,
                 0x7FF,   /* CAN ID */
                 false,   /* extended ID */
                 false,   /* RTR */
                 8,       /* DLC */
                 data);
    msg_counter1++;
}

void canhandler_sendDemoMessage_CAN2(void) {
    static uint16_t msg_counter2 = 0;
    uint8_t data[8] = {
        (uint8_t)(msg_counter2 >> 8),
        (uint8_t)(msg_counter2 & 0xFF),
        0x22, 0x22, 0x22, 0x22, 0x22, 0x22
    };
    can_transmit(CAN2,
                 0x7FF,   /* CAN ID */
                 false,   /* extended ID */
                 false,   /* RTR */
                 8,       /* DLC */
                 data);
    msg_counter2++;
}

/* CAN1 on PA11/PA12, CAN2 on PB5/PB6 (remapped), 500 kBaud
   APB1=36MHz, BRP=4, TS1=13, TS2=4, SJW=1 → sample point 77.8 % */
void canhandler_setup(void)
{
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_AFIO);
    rcc_periph_clock_enable(RCC_CAN1);
    rcc_periph_clock_enable(RCC_CAN2);

    gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ,
                  GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO12); /* CAN1_TX */
    gpio_set_mode(GPIOA, GPIO_MODE_INPUT,
                  GPIO_CNF_INPUT_FLOAT, GPIO11);           /* CAN1_RX */

    /* Remap CAN2 from PB12/PB13 to PB5/PB6 */
    AFIO_MAPR |= AFIO_MAPR_CAN2_REMAP;

    gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ,
                  GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO6); /* CAN2_TX */
    gpio_set_mode(GPIOB, GPIO_MODE_INPUT,
                  GPIO_CNF_INPUT_FLOAT, GPIO5);           /* CAN2_RX */

    can_init(CAN1,
             false,             /* TTCM */
             true,              /* ABOM: auto bus-off recovery */
             false,             /* AWUM */
             false,             /* NART */
             false,             /* RFLM */
             false,             /* TXFP */
             CAN_BTR_SJW_1TQ,
             CAN_BTR_TS1_13TQ, /* BS1 = 13 */
             CAN_BTR_TS2_4TQ,  /* BS2 = 4  */
             4,                 /* prescaler → 500 kBaud */
             false,             /* loopback */
             false);            /* silent   */

    can_init(CAN2,
             false,
             true,
             false,
             false,
             false,
             false,
             CAN_BTR_SJW_1TQ,
             CAN_BTR_TS1_13TQ,
             CAN_BTR_TS2_4TQ,
             4,                 /* prescaler → 500 kBaud */
             false,
             false);

    /* Pass-all filter bank 0 for CAN1, bank 14 for CAN2 (default split) */
    can_filter_id_mask_32bit_init(0,  0, 0, 0, true);
    can_filter_id_mask_32bit_init(14, 0, 0, 0, true);

    can_enable_irq(CAN1, CAN_IER_FMPIE0);
    nvic_enable_irq(NVIC_USB_LP_CAN_RX0_IRQ);

    can_enable_irq(CAN2, CAN_IER_FMPIE0);
    nvic_enable_irq(NVIC_CAN2_RX0_IRQ);
}

void usb_lp_can_rx0_isr(void)
{
    uint32_t id;
    bool     ext, rtr;
    uint8_t  fmi, dlc, data[8];

    can_receive(CAN1, 0, true, &id, &ext, &rtr, &fmi, &dlc, data, NULL);

    uint8_t next = (can1_rx_ring_head + 1) & (CAN1_RX_RING_SIZE - 1);
    if (next != can1_rx_ring_tail) {
        uint8_t h = can1_rx_ring_head;
        can1_rx_ring[h].id  = id;
        can1_rx_ring[h].dlc = dlc;
        for (uint8_t i = 0; i < dlc && i < 8; i++)
            can1_rx_ring[h].data[i] = data[i];
        can1_rx_ring_head = next;
    } else {
        nCan1Dropped++;
        nCan1DroppedFramesPerSecond++;
    }
}

void canhandler_process_rx1(uint32_t now_ms)
{
    while (can1_rx_ring_tail != can1_rx_ring_head) {
        uint8_t  t   = can1_rx_ring_tail;
        can1_rxid  = can1_rx_ring[t].id;
        can1_rxdlc = can1_rx_ring[t].dlc;
        for (uint8_t i = 0; i < 8; i++)
            can1_rxdata[i] = can1_rx_ring[t].data[i];
        can1_rx_ring_tail = (t + 1) & (CAN1_RX_RING_SIZE - 1);

        nCan1TotalReceived++;
        nCan1ReceivedFramesPerSecond++;

        /* route the message to the other CAN */
        canhandler_routeFromCan1ToCan2();

        /* add the message into the table, just for printing over serial */
        int found = -1, empty = -1;
        for (int i = 0; i < CAN1_MSG_TABLE_SIZE; i++) {
            if (can1_msg_table[i].valid && can1_msg_table[i].id == can1_rxid) {
                found = i;
                break;
            }
            if (!can1_msg_table[i].valid && empty < 0)
                empty = i;
        }

        int slot = (found >= 0) ? found : empty;
        if (slot >= 0) {
            can1_msg_table[slot].id           = can1_rxid;
            can1_msg_table[slot].dlc          = can1_rxdlc;
            for (uint8_t i = 0; i < 8; i++)
                can1_msg_table[slot].data[i]  = can1_rxdata[i];
            can1_msg_table[slot].timestamp_ms = now_ms;
            can1_msg_table[slot].rx_count++;
            can1_msg_table[slot].valid        = true;
        }
        /* else: table full — counted in nCan1TotalReceived but discarded */
    }
}

void can2_rx0_isr(void)
{
    uint32_t id;
    bool     ext, rtr;
    uint8_t  fmi, dlc, data[8];

    can_receive(CAN2, 0, true, &id, &ext, &rtr, &fmi, &dlc, data, NULL);

    uint8_t next = (can2_rx_ring_head + 1) & (CAN2_RX_RING_SIZE - 1);
    if (next != can2_rx_ring_tail) {
        uint8_t h = can2_rx_ring_head;
        can2_rx_ring[h].id  = id;
        can2_rx_ring[h].dlc = dlc;
        for (uint8_t i = 0; i < dlc && i < 8; i++)
            can2_rx_ring[h].data[i] = data[i];
        can2_rx_ring_head = next;
    } else {
        nCan2Dropped++;
        nCan2DroppedFramesPerSecond++;
    }
}

void canhandler_process_rx2(uint32_t now_ms)
{
    while (can2_rx_ring_tail != can2_rx_ring_head) {
        uint8_t  t   = can2_rx_ring_tail;
        can2_rxid  = can2_rx_ring[t].id;
        can2_rxdlc = can2_rx_ring[t].dlc;
        for (uint8_t i = 0; i < 8; i++)
            can2_rxdata[i] = can2_rx_ring[t].data[i];
        can2_rx_ring_tail = (t + 1) & (CAN2_RX_RING_SIZE - 1);

        nCan2TotalReceived++;
        nCan2ReceivedFramesPerSecond++;

        /* route the message to the other CAN */
        canhandler_routeFromCan2ToCan1();

        /* add the message into the table, just for printing over serial */
        int found = -1, empty = -1;
        for (int i = 0; i < CAN2_MSG_TABLE_SIZE; i++) {
            if (can2_msg_table[i].valid && can2_msg_table[i].id == can2_rxid) {
                found = i;
                break;
            }
            if (!can2_msg_table[i].valid && empty < 0)
                empty = i;
        }

        int slot = (found >= 0) ? found : empty;
        if (slot >= 0) {
            can2_msg_table[slot].id           = can2_rxid;
            can2_msg_table[slot].dlc          = can2_rxdlc;
            for (uint8_t i = 0; i < 8; i++)
                can2_msg_table[slot].data[i]  = can2_rxdata[i];
            can2_msg_table[slot].timestamp_ms = now_ms;
            can2_msg_table[slot].rx_count++;
            can2_msg_table[slot].valid        = true;
        }
    }
}
