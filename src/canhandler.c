#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/can.h>
#include <libopencm3/cm3/nvic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "canhandler.h"
#include "pedalhandler.h"

#define CAN1_RX_RING_SIZE 32   /* must be power of 2 */
#define CAN2_RX_RING_SIZE 32
/* If no TX mailbox of the destination CAN is free, the frame stays in the RX ring and is
   routed later (bursts at full bus load). Only if the ring is nearly full (e.g. the
   destination bus does not acknowledge), the frame is processed anyway and counted as
   TX dropped, so that the evaluation of the messages does not stop. */
#define RX_RING_RESERVE 4

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
uint32_t          nCan1TxDropped = 0;

can1_msg_entry_t  can2_msg_table[CAN2_MSG_TABLE_SIZE];
uint32_t          nCan2TotalReceived = 0;
volatile uint32_t nCan2Dropped = 0;
volatile uint32_t nCan2ReceivedFramesPerSecond = 0;
volatile uint32_t nCan2DroppedFramesPerSecond = 0;
uint32_t          nCan2TxDropped = 0;


uint32_t can1_rxid;
uint8_t  can1_rxdlc;
uint8_t  can1_rxdata[8];
uint8_t  can1_txdata[8];

uint32_t can2_rxid;
uint8_t  can2_rxdlc;
uint8_t  can2_rxdata[8];
uint8_t  can2_txdata[8];

/* 0x59C B4..B5 (LE16, 0.1 A) is the target current which the CCM forwards 1:1 as
   EVTargetCurrent to the charger (see doc/bench-testing.md). */
#define CAN_ID_TARGET_CURRENT          0x59C

uint32_t nTargetCurrentLimited = 0;

void canhandler_routeFromCan1ToCan2(void) {
    uint8_t i;
    for (i=0; i<8; i++) can2_txdata[i] = can1_rxdata[i]; /* copy originally first */
    /* limit the target current to the value which the driver selected with the pedals */
    if ((can1_rxid == CAN_ID_TARGET_CURRENT) && (can1_rxdlc >= 6)) {
        uint16_t deciAmps = can2_txdata[4] | (can2_txdata[5] << 8);
        uint16_t limit = pedal_currentLimitDeciAmps;
        if (deciAmps > limit) {
            can2_txdata[4] = limit & 0xFF;
            can2_txdata[5] = limit >> 8;
            nTargetCurrentLimited++;
        }
    }
    /* finally transmit the message on the destination CAN */
    if (can_transmit(CAN2,
                     can1_rxid,   /* CAN ID */
                     false,   /* extended ID */
                     false,   /* RTR */
                     can1_rxdlc,       /* DLC */
                     can2_txdata) < 0) {
        nCan2TxDropped++; /* all three TX mailboxes busy */
    }
}

void canhandler_routeFromCan2ToCan1(void) {
    uint8_t i;
    for (i=0; i<8; i++) can1_txdata[i] = can2_rxdata[i]; /* copy originally first */
    /* here is the place to add modifications to the message */
    /* finally transmit the message on the destination CAN */
    if (can_transmit(CAN1,
                     can2_rxid,   /* CAN ID */
                     false,   /* extended ID */
                     false,   /* RTR */
                     can2_rxdlc,       /* DLC */
                     can1_txdata) < 0) {
        nCan1TxDropped++; /* all three TX mailboxes busy */
    }
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

    /* Remap CAN2 from PB12/PB13 to PB5/PB6. The SWJ_CFG bits in AFIO_MAPR are write-only
       (read is undefined), so they must be written explicitly; a read-modify-write may
       disable the SWD on the GD32. Keep SWD on, JTAG off. */
    gpio_primary_remap(AFIO_MAPR_SWJ_CFG_JTAG_OFF_SW_ON, AFIO_MAPR_CAN2_REMAP);

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
             true,              /* TXFP: send in chronological order, not by ID priority */
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
             true,              /* TXFP: chronological order */
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

    /* ext and rtr are ignored: the Ioniq PCAN uses only 11-bit data frames */
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

/* The messages which are stored for the serial output. */
static const uint32_t loggedIds[] = {
    0x59C,  /* vehicle: target voltage and target current (before the limitation) */
    0x599,  /* vehicle: SOC, CCS trigger, stop byte B7 */
    0x200,  /* vehicle: accelerator pedal */
    0x394,  /* vehicle: brake pedal */
    0x5E5,  /* CCM: EVSE maximum and present voltage and current */
    0x5E9,  /* CCM: CCM_ChargeProgress */
};

static bool canhandler_isLoggedId(uint32_t id)
{
    for (uint8_t i = 0; i < sizeof(loggedIds) / sizeof(loggedIds[0]); i++) {
        if (loggedIds[i] == id) return true;
    }
    return false;
}

void canhandler_process_rx1(uint32_t now_ms)
{
    while (can1_rx_ring_tail != can1_rx_ring_head) {
        uint8_t fill = (can1_rx_ring_head - can1_rx_ring_tail) & (CAN1_RX_RING_SIZE - 1);
        if (!can_available_mailbox(CAN2) && (fill < CAN1_RX_RING_SIZE - RX_RING_RESERVE))
            break; /* wait for a free TX mailbox, retry in the next main loop */
        uint8_t  t   = can1_rx_ring_tail;
        can1_rxid  = can1_rx_ring[t].id;
        can1_rxdlc = can1_rx_ring[t].dlc;
        for (uint8_t i = 0; i < 8; i++)
            can1_rxdata[i] = can1_rx_ring[t].data[i];
        can1_rx_ring_tail = (t + 1) & (CAN1_RX_RING_SIZE - 1);

        nCan1TotalReceived++;
        nCan1ReceivedFramesPerSecond++;

        /* evaluate the pedals, before routing, so that the new limit applies at once */
        pedalhandler_onCan1Frame(can1_rxid, can1_rxdlc, can1_rxdata, now_ms);

        /* route the message to the other CAN */
        canhandler_routeFromCan1ToCan2();

        /* add the message into the table, just for printing over serial */
        if (!canhandler_isLoggedId(can1_rxid)) continue;
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

    /* ext and rtr are ignored: the Ioniq PCAN uses only 11-bit data frames */
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
        uint8_t fill = (can2_rx_ring_head - can2_rx_ring_tail) & (CAN2_RX_RING_SIZE - 1);
        if (!can_available_mailbox(CAN1) && (fill < CAN2_RX_RING_SIZE - RX_RING_RESERVE))
            break; /* wait for a free TX mailbox, retry in the next main loop */
        uint8_t  t   = can2_rx_ring_tail;
        can2_rxid  = can2_rx_ring[t].id;
        can2_rxdlc = can2_rx_ring[t].dlc;
        for (uint8_t i = 0; i < 8; i++)
            can2_rxdata[i] = can2_rx_ring[t].data[i];
        can2_rx_ring_tail = (t + 1) & (CAN2_RX_RING_SIZE - 1);

        nCan2TotalReceived++;
        nCan2ReceivedFramesPerSecond++;

        /* detect the start of a charging session */
        pedalhandler_onCan2Frame(can2_rxid, can2_rxdlc, can2_rxdata, now_ms);

        /* route the message to the other CAN */
        canhandler_routeFromCan2ToCan1();

        /* add the message into the table, just for printing over serial */
        if (!canhandler_isLoggedId(can2_rxid)) continue;
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
