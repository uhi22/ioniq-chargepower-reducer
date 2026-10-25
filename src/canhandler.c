#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/can.h>
#include <libopencm3/cm3/nvic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "canhandler.h"

#define CAN1_RX_RING_SIZE 32   /* must be power of 2 */

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} can1_ring_entry_t;

static volatile can1_ring_entry_t can1_rx_ring[CAN1_RX_RING_SIZE];
static volatile uint8_t           can1_rx_ring_head = 0;
static volatile uint8_t           can1_rx_ring_tail = 0;

can1_msg_entry_t  can1_msg_table[CAN1_MSG_TABLE_SIZE];
uint32_t          nCan1TotalReceived = 0;
volatile uint32_t nCan1Dropped = 0;
volatile uint32_t nCan1ReceivedFramesPerSecond = 0;
volatile uint32_t nCan1DroppedFramesPerSecond = 0;

/* CAN1 on PA11/PA12, 500 kBaud
   APB1=36MHz, BRP=4, TS1=13, TS2=4, SJW=1 → sample point 77.8 % */
void canhandler_setup(void)
{
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_AFIO);
    rcc_periph_clock_enable(RCC_CAN1);

    gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ,
                  GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO12); /* CAN1_TX */
    gpio_set_mode(GPIOA, GPIO_MODE_INPUT,
                  GPIO_CNF_INPUT_FLOAT, GPIO11);           /* CAN1_RX */

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

    /* Pass-all filter: mask=0 accepts every ID into FIFO0 */
    can_filter_id_mask_32bit_init(0, 0, 0, 0, true);

    can_enable_irq(CAN1, CAN_IER_FMPIE0);
    nvic_enable_irq(NVIC_USB_LP_CAN_RX0_IRQ);
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

void canhandler_process_rx(uint32_t now_ms)
{
    while (can1_rx_ring_tail != can1_rx_ring_head) {
        uint8_t  t   = can1_rx_ring_tail;
        uint32_t id  = can1_rx_ring[t].id;
        uint8_t  dlc = can1_rx_ring[t].dlc;
        uint8_t  data[8];
        for (uint8_t i = 0; i < 8; i++)
            data[i] = can1_rx_ring[t].data[i];
        can1_rx_ring_tail = (t + 1) & (CAN1_RX_RING_SIZE - 1);

        nCan1TotalReceived++;
        nCan1ReceivedFramesPerSecond++;

        int found = -1, empty = -1;
        for (int i = 0; i < CAN1_MSG_TABLE_SIZE; i++) {
            if (can1_msg_table[i].valid && can1_msg_table[i].id == id) {
                found = i;
                break;
            }
            if (!can1_msg_table[i].valid && empty < 0)
                empty = i;
        }

        int slot = (found >= 0) ? found : empty;
        if (slot >= 0) {
            can1_msg_table[slot].id           = id;
            can1_msg_table[slot].dlc          = dlc;
            for (uint8_t i = 0; i < 8; i++)
                can1_msg_table[slot].data[i]  = data[i];
            can1_msg_table[slot].timestamp_ms = now_ms;
            can1_msg_table[slot].rx_count++;
            can1_msg_table[slot].valid        = true;
        }
        /* else: table full — counted in nCan1TotalReceived but discarded */
    }
}
