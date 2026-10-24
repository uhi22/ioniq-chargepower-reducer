BINARY      = chargepower-reducer
OPENCM3_DIR = libopencm3
LDSCRIPT    = stm32f105rb.ld

SRCS = src/main.c
OBJS = $(SRCS:.c=.o)

# stm32f105rb: Cortex-M3, soft-float, 128K flash, 64K RAM
ARCH_FLAGS  = -mcpu=cortex-m3 -mthumb -msoft-float
CPPFLAGS   += -DSTM32F1 -DSTM32F105RB
CPPFLAGS   += -I$(OPENCM3_DIR)/include
CFLAGS     += -Wall -Wextra -Os -std=c99
LDFLAGS    += --static -nostartfiles -L$(OPENCM3_DIR)/lib
LDLIBS     += -lopencm3_stm32f1 -Wl,--start-group -lc -lgcc -Wl,--end-group
LIBDEPS     = $(OPENCM3_DIR)/lib/libopencm3_stm32f1.a

include $(OPENCM3_DIR)/mk/gcc-config.mk

.PHONY: all clean

all: $(BINARY).elf $(BINARY).bin

clean:
	rm -f $(OBJS) $(BINARY).elf $(BINARY).bin $(BINARY).hex

include $(OPENCM3_DIR)/mk/gcc-rules.mk
