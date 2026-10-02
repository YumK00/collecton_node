#include "interrupt.h"
#include "FreeRTOS.h"
#include "semphr.h"

/* ============================================================
 * 【外部中断 + 信号量】—— RTOS 里"中断通知任务"的标准写法
 *
 * 数据流：
 *   按下按键(PB12) → 硬件触发下降沿 → 进 EXTI15_10_IRQHandler
 *      → HAL_GPIO_EXTI_IRQHandler 清标志位 → 自动调用 HAL_GPIO_EXTI_Callback
 *      → xSemaphoreGiveFromISR 把信号量给出去
 *      → photoAdcTask 里那句阻塞的 xSemaphoreTake 被唤醒 → 采一次 ADC
 *
 * 为什么要用中断，不再用 cmdTask 每 1 秒轮询一次？
 *   轮询：CPU 每秒醒一次去读引脚，没按键也白跑，按键最多要等 1 秒才被响应。
 *   中断：平常 CPU 完全不管，按键一按下立刻响应(微秒级)，且不占 CPU。
 * ============================================================ */

/* xBinarySem 的"定义"在 freertos.c 里，这里只是"声明"一下，
 * 告诉编译器：这个变量别的地方有，先让我用，链接时再去找。
 * 这就是 extern 的用途 —— 定义放 .c(只能一处)，声明放 .h 或别的 .c。 */
extern SemaphoreHandle_t xBinarySem;

/**
  * @brief  初始化 PB12 按键，配成下降沿触发的 EXTI 中断
  * @note   PB12 接了按键，另一端接 GND：
  *         松开时被内部上拉电阻拉到 高电平
  *         按下时直接连到 GND，变成 低电平 —— 这个"高→低"就是下降沿
  */
void Key_Init(void)
{
    /* 开 GPIOB 的时钟。不开时钟的话，下面写 GPIOB 的寄存器全部无效，
     * 引脚不会有任何变化 —— 这是新手最常踩的坑。 */
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin   = GPIO_PIN_12;
    gpio.Mode  = GPIO_MODE_IT_FALLING;   // IT = InTerrupt，下降沿触发中断
    gpio.Pull  = GPIO_PULLUP;            // 内部上拉，没按键时保持高电平
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;

    /* 这一句内部会自动做两件事(见 stm32f1xx_hal_gpio.c:288-295)：
     *   ① 使能 AFIO 时钟
     *   ② 写 AFIO->EXTICR，把 EXTI12 这条线接到 GPIOB 上
     * 因为 EXTI 线是共用的：EXTI12 可以是 PA12/PB12/PC12...，
     * 必须告诉 AFIO 到底听哪个端口。所以这里不用手动配 AFIO。 */
    HAL_GPIO_Init(GPIOB, &gpio);

    /* ---- NVIC 配置：让 CPU 真的能收到这个中断 ----
     *
     * ★优先级必须 >= 5，不能是 0★
     *   FreeRTOSConfig.h 里设了 configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5。
     *   规矩是：凡是中断里要调用 "...FromISR()" 这类 FreeRTOS API 的中断，
     *   优先级数值必须 >= 5(数值越大优先级越低)。
     *   等于 5 说明"你比我高一点点，但还在我能屏蔽的范围内"，内核才管得住。
     *   如果设成 0(最高优先级)，内核屏蔽不住它，而它又去调 FreeRTOS 内核函数，
     *   会把内核的链表改坏 —— 所以 FreeRTOS 在启动时就 configASSERT 报错拦下来。
     *   (你之前那版草稿写的 SetPriority(...,0,0) 和 SetPriorityGrouping(2)
     *    两条都会触发这个错误。)
     *
     * ★不要自己调 HAL_NVIC_SetPriorityGrouping★
     *   HAL_Init() 已经设成 NVIC_PRIORITYGROUP_4(4位全是抢占优先级)，
     *   这正是 FreeRTOS 要求的。改成 GROUP_2 会分掉 2 位给子优先级，
     *   内核一样会在启动时断言失败。 */
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

/**
  * @brief  EXTI 线 10~15 共用的中断服务函数
  * @note   名字必须和启动文件里的向量表名字一模一样，
  *         否则会去跑启动文件里的弱定义(死循环)。
  *         PB12 → EXTI12 → 属于 10~15 这一组。
  */
void EXTI15_10_IRQHandler(void)
{
    /* 传进去的 12 表示"我关心 EXTII12 这条线"。
     * 这个函数会：清掉挂起标志位 → 调用下面的 HAL_GPIO_EXTI_Callback。
     * 注意它不区分是谁触发的，真正的判断在回调里做。 */
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_12);
}

/**
  * @brief  GPIO 外部中断回调(HAL 的弱函数，这里把它重写掉)
  * @note   ★打断点/写逻辑都写这里，不要写在 EXTI15_10_IRQHandler 里★
  *         因为 10~15 这几条线共用那一个中断入口，
  *         HAL 帮你清完标志后统一调到这里，你再按 Pin 号分流。
  */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_12)
    {
        /* ---- 中断里给信号量，必须用 FromISR 版本 ----
         *
         * xSemaphoreGiveFromISR 和 xSemaphoreGive 的区别：
         *   普通版会去操作"等待链表"、判断要不要立刻切换任务，甚至可能
         *   自己阻塞 —— 中断里绝对不能阻塞，所以普通版禁止在中断里用。
         *   FromISR 版是专门改写的"不阻塞"版本。
         *
         * 第二个参数 xHigherPriorityTaskWoken 是"出参"：
         *   如果这一给，唤醒了一个比当前正在跑的任务优先级更高的任务，
         *   内核会把它填成 pdTRUE，意思是"出了中断请立刻切过去"。
         *   pdFALSE 是初值，表示"先假设不用切"。 */
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        /* ★为什么要判空★ —— 一个真实的时序坑：
         * main() 里的顺序是 Key_Init()(开中断) → MX_FREERTOS_Init()(建信号量)。
         * 开中断之后、信号量还没创建之前，xBinarySem 还是 NULL。
         * 万一正好在这段窗口里按了键，xSemaphoreGiveFromISR(NULL,...)
         * 会撞上 FreeRTOS 内部的 configASSERT，直接死循环卡住。
         * 加一句判空，这个窗口就安全了(而且顺便养成习惯：
         * 中断里用任何 RTOS 对象前，先确认它已经被创建)。 */
        if (xBinarySem != NULL)
        {
            xSemaphoreGiveFromISR(xBinarySem, &xHigherPriorityTaskWoken);
        }

        /* ★这一句不能省★
         * 中断函数不是普通函数，它没法自己"退出时切任务"。
         * portYIELD_FROM_ISR 会去请求一次 PendSV 异常，
         * 等这个中断函数返回后，CPU 立刻进 PendSV 做任务切换。
         * 如果省略，被唤醒的高优先级任务要等到下一次
         * 系统节拍(1ms)才可能被调度，响应就慢了。
         * 参数就是上面那个变量：pdTRUE 才真的切，pdFALSE 就什么都不做。 */
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}
