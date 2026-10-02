#include "adc_dma.h"
#include "stm32f103xb.h"
#include "stm32f1xx_hal_gpio.h"
#include "stm32f1xx_hal_rcc.h"
#include "stm32f1xx_hal_rcc_ex.h"   // __HAL_RCC_ADC_CONFIG / RCC_ADCPCLK2_DIVx
#include "stm32f1xx_hal_adc_ex.h"   // HAL_ADCEx_Calibration_Start
#include "stm32f1xx_hal_dma.h"      // DMA_HandleTypeDef / __HAL_LINKDMA

// ADC1句柄：连续扫描+DMA
ADC_HandleTypeDef hadc_dma;
// DMA1_Channel1句柄：F103的ADC1固定映射到DMA1通道1
DMA_HandleTypeDef hdma_adc1;

// DMA循环缓冲区：DMA自动把CH0写[0]、CH1写[1]，循环覆盖，主循环直接读
volatile uint16_t adc_dma_buf[ADC_DMA_CH_NUM] = {0};

/**
 * @brief  ADC1双通道连续扫描 + DMA，读取PA0(IN0)、PA1(IN1)两个模拟量
 * @note   工作流程：
 *         1. 扫描模式(ScanConvMode=ENABLE)：依次转换CH0→CH1；
 *         2. 连续模式(ContinuousConvMode=ENABLE)：转完一轮自动从头再转；
 *         3. 每转完一个通道，ADC触发一次DMA请求，DMA把DR里的结果搬到内存
            (因为规则组可以配置同时16个数据通道，但是只有一个规则通道数据寄存器，
            也就是一次可以给16道菜，但桌子只能放一道菜，所以放一道菜就叫DMA拿走一道)
 *         4. DMA循环模式(CIRCULAR)：buf[0]存CH0、buf[1]存CH1，写满自动回头覆盖，
 *            主循环随时读buf即可，CPU不参与搬运。
 *
 *         时钟：APB2=72MHz，ADCPRE分频6 → ADCCLK=72/6=12MHz ≤ 14MHz(F1上限)。
 * @retval None
 */
void ADC_DMA_Init(void)
{
    // 开启GPIOA(模拟引脚)、ADC1(ADC)、DMA1(DMA搬运)三个外设时钟
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    // ADC时钟分频：APB2=72MHz / 6 = 12MHz，F1的ADC时钟不能超过14MHz
    __HAL_RCC_ADC_CONFIG(RCC_ADCPCLK2_DIV6);

    // ==========GPIO配置：PA0、PA1模拟输入==========
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;  // PA0=ADC1_IN0、PA1=ADC1_IN1
    gpio.Mode = GPIO_MODE_ANALOG;        // 模拟输入：关闭数字输入电路
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    // ==========ADC工作模式：扫描 + 连续==========
    hadc_dma.Instance = ADC1;
    hadc_dma.Init.ScanConvMode = ADC_SCAN_ENABLE;        // 扫描模式：多通道逐个转
    hadc_dma.Init.ContinuousConvMode = ENABLE;           // 连续模式：转完一轮自动再来
    hadc_dma.Init.DiscontinuousConvMode = DISABLE;       // 关闭间断模式
    hadc_dma.Init.ExternalTrigConv = ADC_SOFTWARE_START; // 软件触发,也就是HAL_ADC_Start_xx才开始
    hadc_dma.Init.DataAlign = ADC_DATAALIGN_RIGHT;       // 右对齐：结果0~4095
    hadc_dma.Init.NbrOfConversion = 2;      // 每轮转换2个通道
    HAL_ADC_Init(&hadc_dma);

    // ==========ADC自校准==========
    // F1首次使用前必须校准，消除内部采样电容误差，否则读数有固定偏移
    HAL_ADCEx_Calibration_Start(&hadc_dma);

    // ==========规则组序列：CH0=Rank1、CH1=Rank2==========
    ADC_ChannelConfTypeDef sConfig = {0};
    sConfig.Channel = ADC_CHANNEL_0;                 // 通道0 = PA0
    sConfig.Rank = ADC_REGULAR_RANK_1;               // 第1个转换
    sConfig.SamplingTime = ADC_SAMPLETIME_55CYCLES_5; // 采样时间55.5个周期
    HAL_ADC_ConfigChannel(&hadc_dma, &sConfig);

    sConfig.Channel = ADC_CHANNEL_1;                 // 通道1 = PA1
    sConfig.Rank = ADC_REGULAR_RANK_2;               // 第2个转换
    HAL_ADC_ConfigChannel(&hadc_dma, &sConfig);

    // ==========DMA配置：DMA1_Channel1==========
    hdma_adc1.Instance = DMA1_Channel1;               // F103 ADC1固定映射DMA1通道1
    hdma_adc1.Init.Direction = DMA_PERIPH_TO_MEMORY;  // 方向：外设(ADC) → 内存(buf)
    hdma_adc1.Init.PeriphInc = DMA_PINC_DISABLE;      // 外设地址不变：一直读同一个ADC_DR
    hdma_adc1.Init.MemInc = DMA_MINC_ENABLE;          // 内存地址递增：buf[0]→buf[1]
    hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD; // 每次搬半个字(16位)
    hdma_adc1.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;    // 内存侧也是16位对齐
    hdma_adc1.Init.Mode = DMA_CIRCULAR;               // 循环模式：写满自动回头覆盖
    hdma_adc1.Init.Priority = DMA_PRIORITY_HIGH;      // 优先级：高
    HAL_DMA_Init(&hdma_adc1);

    // 把DMA句柄挂到ADC句柄上，HAL_ADC_Start_DMA内部要用
    __HAL_LINKDMA(&hadc_dma, DMA_Handle, hdma_adc1);

    // 启动连续转换+DMA(连续模式只启动一次，DMA循环搬运不停，无需在循环里反复启动)
    HAL_ADC_Start_DMA(&hadc_dma, (uint32_t *)adc_dma_buf, ADC_DMA_CH_NUM);
}

/**
 * @brief  读取某通道的ADC原始值
 * @param  ch：通道号，0=PA0(IN0)、1=PA1(IN1)
 * @note   直接把DMA搬进内存的原始值返回，不做任何换算。
 *         要电压(mV)就自己算：mv = 原值 * 3300 / 4095。
 * @retval uint16_t：ADC原始值(0~4095)
 */
uint16_t ADC_DMA_GetValue(uint8_t ch)
{
    if (ch >= ADC_DMA_CH_NUM)
    {
        return 0;                                    // 非法通道号，返回0
    }
    return adc_dma_buf[ch];
}
