#include "co2.h"
#include "serial.h"

// 注：这里不需要 extern huart2。co2.c 只调用 serial.c 提供的
// Serial_Init / Serial_Receive 接口，不直接碰句柄，句柄由 serial.c 内部持有。

void co2_Init(void){
    Serial_Init();
}

/**
 * @brief  读一次CO2浓度
 * @param  pRxData：调用者提供的6字节缓冲区
 * @retval CO2浓度(ppm)；出错(超时/校验失败)返回 0xFFFF
 * @note   帧格式：6字节 [模块地址][CO2高位][CO2低位][满量程高位][满量程低位][校验和]
 *         例：2C 03 D9 03 FF 0A
 *           [0]=2C 模块地址
 *           [1]=03 CO2高位  ┐ CO2     = 0x03D9 = 985 ppm
 *           [2]=D9 CO2低位  ┘
 *           [3]=03 满量程高位 ┐ 满量程 = 0x03FF = 1023 ppm
 *           [4]=FF 满量程低位 ┘
 *           [5]=0A 校验和 = 前5个字节累加的低8位
 */
uint16_t co2_Read(uint8_t *pRxData){
    // 第1步：收数据。必须检查返回值——超时的话 pRxData 里是旧数据，
    // 后面拿它算校验和可能"凑巧"通过，把垃圾当有效值用。
    if (Serial_Receive(pRxData, 6, 10000) != HAL_OK)
    {
        return 0x0000;                     // 500ms内没收满6字节
    }

    // 第2步：校验和。前5字节累加后要 & 0xFF，只留低8位。
    uint8_t sum = pRxData[0] + pRxData[1] + pRxData[2] + pRxData[3] + pRxData[4];
    if (sum != pRxData[5])
    {
        return 0xFFFF;     
    }

    // 第3步：浓度 = CO2高位<<8 | CO2低位
    // CO2高位在 pRxData[1]、CO2低位在 pRxData[2]。
    return (uint16_t)(pRxData[1] << 8) | pRxData[2];
}