#ifndef __CO2_H
#define __CO2_H
#include "serial.h"

void co2_Init(void);
uint16_t co2_Read(uint8_t *pRxData);

#endif /* __CO2_H */