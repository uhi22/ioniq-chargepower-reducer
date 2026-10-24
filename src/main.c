#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/stm32/can.h>

/* 25 MHz HSE → PLL2 chain → 72 MHz SYSCLK, APB1 = 36 MHz */
static void clock_setup(void)
{
    rcc_clock_setup_pll(&rcc_hse_configs[RCC_CLOCK_HSE25_72MHZ]);
}

/* USART3 on PB10 (solder pad P2.3), 115200 8N1 */
static void usart_setup(void)
{
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_AFIO);
    rcc_periph_clock_enable(RCC_USART3);

    gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ,
                  GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO10);

    usart_set_baudrate(USART3, 115200);
    usart_set_databits(USART3, 8);
    usart_set_stopbits(USART3, USART_STOPBITS_1);
    usart_set_parity(USART3, USART_PARITY_NONE);
    usart_set_flow_control(USART3, USART_FLOWCONTROL_NONE);
    usart_set_mode(USART3, USART_MODE_TX);
    usart_enable(USART3);
}

/* CAN1 on PA11/PA12 (default mapping), 500 kBaud
   APB1=36MHz, BRP=4, TS1=13, TS2=4, SJW=1 → sample point 77.8 % */
static void can_setup(void)
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
}

static void usart_puts(const char *s)
{
    while (*s)
        usart_send_blocking(USART3, *s++);
}

int main(void)
{
    clock_setup();
    usart_setup();
    can_setup();

    usart_puts("Hello World\r\n");

    while (1);
    return 0;
}
