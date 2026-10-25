#ifndef SERIALHANDLER_H
#define SERIALHANDLER_H

#include <stdint.h>

extern uint32_t nSerialTxDrop;

void serialhandler_setup(void);
void serialhandler_puts(const char *s);
void serialhandler_print_uint32(uint32_t n);
void serialhandler_print_hex8(uint8_t v);
void serialhandler_print_hex32(uint32_t v);

#endif /* SERIALHANDLER_H */
