#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/cm3/nvic.h>
#include <stdint.h>
#include "serialhandler.h"

#define SERIALHANDLER_TX_RING_SIZE 8192   /* must be power of 2 */

static volatile uint8_t  serialhandler_tx_ring[SERIALHANDLER_TX_RING_SIZE];
static volatile uint16_t serialhandler_tx_ring_head = 0;
static volatile uint16_t serialhandler_tx_ring_tail = 0;

uint32_t nSerialTxDrop = 0;

/* USART3 on PB10, 115200 8N1 */
void serialhandler_setup(void)
{
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_AFIO);
    rcc_periph_clock_enable(RCC_USART3);

    gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ,
                  GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO10);

    usart_set_baudrate(USART3, 230400);
    usart_set_databits(USART3, 8);
    usart_set_stopbits(USART3, USART_STOPBITS_1);
    usart_set_parity(USART3, USART_PARITY_NONE);
    usart_set_flow_control(USART3, USART_FLOWCONTROL_NONE);
    usart_set_mode(USART3, USART_MODE_TX);
    usart_enable(USART3);
    nvic_enable_irq(NVIC_USART3_IRQ);
}

void usart3_isr(void)
{
    if (serialhandler_tx_ring_head != serialhandler_tx_ring_tail) {
        usart_send(USART3, serialhandler_tx_ring[serialhandler_tx_ring_tail]);
        serialhandler_tx_ring_tail = (serialhandler_tx_ring_tail + 1) & (SERIALHANDLER_TX_RING_SIZE - 1);
    } else {
        usart_disable_tx_interrupt(USART3);
    }
}

static void serialhandler_tx_ring_put(uint8_t c)
{
    uint16_t next = (serialhandler_tx_ring_head + 1) & (SERIALHANDLER_TX_RING_SIZE - 1);
    if (next == serialhandler_tx_ring_tail) {
        nSerialTxDrop++;
        return;
    }
    serialhandler_tx_ring[serialhandler_tx_ring_head] = c;
    serialhandler_tx_ring_head = next;
    usart_enable_tx_interrupt(USART3);
}

void serialhandler_puts(const char *s)
{
    while (*s)
        serialhandler_tx_ring_put((uint8_t)*s++);
}

void serialhandler_print_uint32(uint32_t n)
{
    char buf[12];
    int i = sizeof(buf) - 1;
    buf[i] = '\0';
    if (n == 0) {
        buf[--i] = '0';
    } else {
        while (n && i > 0) {
            buf[--i] = '0' + (n % 10);
            n /= 10;
        }
    }
    serialhandler_puts(&buf[i]);
}

void serialhandler_print_hex8(uint8_t v)
{
    const char hex[] = "0123456789ABCDEF";
    serialhandler_tx_ring_put(hex[v >> 4]);
    serialhandler_tx_ring_put(hex[v & 0xF]);
}

void serialhandler_print_hex32(uint32_t v)
{
    serialhandler_print_hex8(v >> 24);
    serialhandler_print_hex8(v >> 16);
    serialhandler_print_hex8(v >> 8);
    serialhandler_print_hex8(v);
}
