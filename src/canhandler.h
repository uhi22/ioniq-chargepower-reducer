#ifndef CANHANDLER_H
#define CANHANDLER_H

#include <stdint.h>
#include <stdbool.h>

/* Ioniq 28 kWh bus has 86 distinct IDs (ref: IONIQ_PCAN_leave_car_while_drive_ready_beep.csv).
   Set to 128 to cover all of them with headroom. */
#define CAN1_MSG_TABLE_SIZE 128

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
    uint32_t timestamp_ms;
    uint32_t rx_count;
    bool     valid;
} can1_msg_entry_t;

extern can1_msg_entry_t  can1_msg_table[CAN1_MSG_TABLE_SIZE];
extern volatile uint32_t nCan1Dropped;
extern volatile uint32_t nCan1ReceivedFramesPerSecond;
extern volatile uint32_t nCan1DroppedFramesPerSecond;
extern uint32_t          nCan1TotalReceived;

void canhandler_setup(void);
void canhandler_process_rx(uint32_t now_ms);

#endif /* CANHANDLER_H */
