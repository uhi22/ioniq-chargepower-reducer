#ifndef CANHANDLER_H
#define CANHANDLER_H

#include <stdint.h>
#include <stdbool.h>

/* Only the messages of interest are stored for the serial output (see canhandler_isLoggedId),
   to keep the processing time per frame and the printing time small at full bus load
   (~2240 frames/s on the Ioniq PCAN). */
#define CAN1_MSG_TABLE_SIZE 8
#define CAN2_MSG_TABLE_SIZE 8

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
extern uint32_t          nCan1TxDropped;

extern can1_msg_entry_t  can2_msg_table[CAN2_MSG_TABLE_SIZE];
extern volatile uint32_t nCan2Dropped;
extern volatile uint32_t nCan2ReceivedFramesPerSecond;
extern volatile uint32_t nCan2DroppedFramesPerSecond;
extern uint32_t          nCan2TotalReceived;
extern uint32_t          nCan2TxDropped;

extern uint32_t          nTargetCurrentLimited;

void canhandler_setup(void);
void canhandler_process_rx1(uint32_t now_ms);
void canhandler_process_rx2(uint32_t now_ms);
extern void canhandler_sendDemoMessage_CAN1(void);
extern void canhandler_sendDemoMessage_CAN2(void);

#endif /* CANHANDLER_H */
