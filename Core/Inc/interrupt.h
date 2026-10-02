#ifndef __INTERRUPT_H
#define __INTERRUPT_H
#include "stm32f1xx_hal.h"

/* 按键(PB12)外部中断初始化。
 * 配好之后，PB12 每来一个"下降沿"(按下)，就会自动进中断，
 * 由中断服务函数 EXTI15_10_IRQHandler 处理。 */
void Key_Init(void);

#endif /* __INTERRUPT_H */
