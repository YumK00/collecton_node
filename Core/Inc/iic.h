#ifndef __IIC_H
#define __IIC_H
#include <stdint.h>
void MyIIC_Init(void);
void MyIIC_Start(void);
void MyIIC_Stop(void);
void MyIIC_SendByte(uint8_t byte);
uint8_t MyIIC_ReceiveByte(void);
void MyIIC_SendAck(uint8_t ack);
uint8_t MyIIC_WaitAck(void);

#endif /* __IIC_H */

