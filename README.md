# 数据采集节点（collecton_node）

> 工业边缘智能网关项目的 **CAN 节点 A**（`NODE_A=1`）：STM32F103 + FreeRTOS，负责红外/光敏两路模拟量采集、周期上报与心跳保活。
> 它是「现场采集 → 边缘网关 → 云端」链路的最前端，只做"把现场变成可传输的数字"这一件事。
> 与 `control_node`（执行节点 B）、`gateway_node`（ESP32-S3 网关）共用同一份 `protocol.h`（v3.0）。

## 一、项目简介
工业现场的传感器数据要进云端，中间隔着一条抗干扰、可多主、带优先级的 CAN 总线。本项目把它拆成三个嵌入式节点：A 节点负责"采"和"报"，B 节点负责"执行"，ESP32-S3 网关负责"转发与上云"。本仓库是其中的采集端：两路 12 位 ADC 由 DMA 循环搬运，采集任务每 100 ms 组一帧 `0x101` 广播出去，心跳任务每 1 s 发一帧 `0x701`；同时它在总线上当"听众"，靠 B 节点和网关的心跳判断谁还活着，并把链路状态画在一块 SSD1306 OLED 上。

现有能力（均为代码中真实存在的功能）：

- **ADC1 双通道扫描 + 连续转换 + DMA 循环**：PA0(IN0)=红外、PA1(IN1)=光敏，CPU 不搬数，任务直接读 `adc_dma_buf[]`。
- **CAN 2.0A 标准帧 250 kbps**：发送查空闲邮箱，接收走 RX FIFO0 中断 → 队列 → 任务，中断里不解析。
- **周期上报** `0x101`（DLC=5，红外/光敏大端 + seq，100 ms）与**心跳** `0x701`（魔数 `0xA5` + 计数，1 s）；收 `0x702`/`0x703` 判活，3 s 超时置离线。
- **下行监听**：`0x000` 急停广播、`0x103` 网关状态（WiFi/MQTT/缓存条数）—— 目前只上屏，不驱动动作。
- **人机接口**：PB12 按键（EXTI + 20 ms tick 消抖）翻转采样开关；SSD1306 128×64 软件 I2C 显示；USART2 115200 打印。
- **FreeRTOS**：5 任务、2 队列、8 KB heap_4 堆，任务创建失败即进 `Error_Handler()`。

## 二、系统架构

```text
                     ┌─────────────────────────────────────────┐
  红外/光敏 ─ADC1────►│  collecton_node ★  节点 A (1)           │
                     │  STM32F103xB + FreeRTOS 10.3.1          │
                     └───────────────────┬─────────────────────┘
                                         │ CAN 2.0A  250 kbps  大端
        ┌────────────────────────────────┼────────────────────────────────┐
        ▼                                ▼                                ▼
  control_node 节点 B (2)         gateway_node 网关 (3)           0x000 急停广播
  电机/执行器执行                   CAN ↔ WiFi/MQTT 双向桥接        任一节点可发
                                         │ MQTT → MQTTX / 手机端 / 云端
```

本仓库在系统里的位置（★ 为本仓库）：

| 仓库 | 节点号 | 平台 | 职责 |
|---|---|---|---|
| **collecton_node ★** | A = 1 | STM32F103 + FreeRTOS | 红外/光敏采集、`0x101` 周期上报、`0x701` 心跳、判活与本地显示 |
| control_node | B = 2 | STM32F103 + FreeRTOS | 电机/执行器控制、`0x102` 状态上报；实际解析 `0x302`（`0x312`/`0x322`/`0x332` 未实现） |
| gateway_node | GW = 3 | ESP32-S3（ESP-IDF） | CAN ↔ WiFi/MQTT 桥接、心跳判活与统计，下发 `0x301`/`0x311`（断网本地缓存为后续阶段） |
| v_car | — | Qt / C++ | 独立项目：车载域 T-Box 仿真，自有 `car/ecu/*` 主题与 Broker，不在本总线上 |

## 三、目录结构

```text
collecton_node/
├── Core/Inc/
│   ├── main.h  gpio.h  FreeRTOSConfig.h  stm32f1xx_hal_conf.h  stm32f1xx_it.h
│   ├── protocol.h   ★ CAN 应用层协议 v3.0（纯头文件，三端共用）
│   ├── can.h  adc_dma.h  serial.h  interrupt.h  oled.h  iic.h  OLED_Font.h
│   ├── oneWire.h  DHT22.h                       单总线温湿度（未接入）
│   └── adc.h  i2c.h  spi.h  spii.h  pwm_ic.h  pwm_oc.h  timer_*.h   ← 整文件被注释
└── Core/Src/
    ├── main.c  freertos.c    时钟树、外设初始化、★ 5 任务 + 2 队列、应用逻辑
    ├── can.c  adc_dma.c  serial.c  interrupt.c  gpio.c  oled.c  iic.c  oneWire.c  DHT22.c
    ├── stm32f1xx_hal_msp.c  stm32f1xx_hal_timebase_tim.c  stm32f1xx_it.c
    ├── system_stm32f1xx.c  syscalls.c  sysmem.c（后两个是 GCC/newlib 支撑）
    └── adc.c  i2c.c  spi.c  spii.c  pwm_ic.c  pwm_oc.c  timer_*.c      ← 整文件被注释
```

**说明**：仓库只保留 CubeMX 生成的 `Core/Inc`（23 个）与 `Core/Src`（25 个）共 48 个文件；HAL 驱动、CMSIS、FreeRTOS 内核、启动文件、链接脚本、`.ioc`、Makefile、工程文件全部没有。`Src` 的 25 个 `.c` 里有 8 个（`adc.c`/`i2c.c`/`spi.c`/`spii.c`/`pwm_ic.c`/`pwm_oc.c`/`timer_*.c`）连同对应头文件**整文件被 `//` 注释**，是空翻译单元——能编过，但没有任何功能。

## 四、核心模块说明
### 4.1 FreeRTOS 任务

| 任务 | 优先级 | 栈 | 周期 | 职责 |
|---|---|---|---|---|
| `photoAdcTask` | 32 (`osPriorityAboveNormal`) | 128 字 / 512 B | 100 ms | 读 ADC 两通道，组 `0x101` 并发送；开关关闭时只空转 |
| `canRxTask` | 24 (`osPriorityNormal`) | 256 字 / 1 KB | 500 ms 超时 | 取接收队列：`0x000`/`0x103`/`0x702`/`0x703`，刷 OLED，心跳判活 |
| `serialSendTask` | 16 (`osPriorityBelowNormal`) | 256 字 / 1 KB | 阻塞等队列 | 拼 `t=...,IR=...,LDR=...` 走串口并刷 OLED 数字 |
| `canHeartTask` | 15 (`osPriorityLow7`) | 256 字 / 1 KB | 1000 ms | 发 `0x701`：魔数 `0xA5` + 自增计数 |
| `StartDefaultTask` | 8 (`osPriorityLow`) | 128 字 / 512 B | 2000 ms | 翻转 PC13，证明调度器在跑 |

`osThreadAttr_t.stack_size` 单位是**字节**，`128 * 4` 等价于 `xTaskCreate` 的 128（字）；CMSIS 的 `osPriorityXxx` 在 `osThreadNew` 里直接透传成内核优先级，所以 32/24/16/15/8 就是真实值（`configMAX_PRIORITIES=56`）。5 个句柄都判了 `NULL`。

### 4.2 队列

| 名称 | 深度 | 元素大小 | 生产者 → 消费者 | 用途 |
|---|---|---|---|---|
| `canRxQueue` | 8 | 16 B（`CanFrame`，含对齐） | CAN RX0 中断 → `canRxTask` | 整帧投递，避免"读到一半被新帧打断" |
| `xQueue` | 8 | 4 B（`s_serialData`） | 无（生产者已被注释）→ `serialSendTask` | 串口/OLED 显示数据 |

没有互斥量、没有信号量（`RTOS_MUTEX`/`RTOS_SEMAPHORES` 段为空），也没有创建任何软件定时器。

### 4.3 源文件职责

| 文件 | 职责 | 状态 |
|---|---|---|
| `main.c` | `HAL_Init` → 时钟 → GPIO/OLED/ADC/串口/LED/按键/CAN → `osKernelStart` | 启用 |
| `freertos.c` | 任务与队列、`0x101`/`0x701` 发送、下行解析、心跳判活 | 启用（应用逻辑全在此） |
| `can.c` / `adc_dma.c` | CAN1 初始化、过滤器、`CAN_Send()`、RX0 回调；ADC1 扫描+连续+DMA | 启用 |
| `iic.c` / `oled.c` / `OLED_Font.h` | 软件 I2C（DWT 计时）、SSD1306 命令/数据/字模/绘图缓冲 | 启用 |
| `serial.c` | USART2 阻塞收发 + `0xAA 0x55 len data sum` 自定义帧 | 启用（发送任务无数据源） |
| `interrupt.c` / `gpio.c` | PB12 EXTI 下降沿 + 20 ms 消抖；`MX_GPIO_Init()` 开时钟、`LED_Init()` 配 PB15/PA8 | 启用 |
| `stm32f1xx_hal_msp.c` / `_timebase_tim.c` / `_it.c` | 引脚与 NVIC、TIM4 做 1 ms HAL 时基、异常向量 | 启用 |
| `protocol.h` | 帧 ID/DLC/字节布局、大端编解码、`PROTO_SeqLost()` | 启用（纯头文件） |
| `oneWire.c` / `DHT22.c` | PC15 单总线时序、DHT22 起停与 5 字节读取 | 已写但**无调用者**，缺 40 bit 解析/校验和 |
| `adc.c` `i2c.c` `spi.c` `spii.c` `pwm_ic.c` `pwm_oc.c` `timer_*.c` | 单次 ADC、I2C2+MPU6050、软件 SPI、SPI1+W25Q64、TIM2 输入捕获、TIM1 PWM、TIM2 定时/ETR 计数 | **整文件被注释，不参与编译** |

### 4.4 三条关键链路

- **CAN 接收**：`USB_LP_CAN1_RX0_IRQHandler` → `HAL_CAN_RxFifo0MsgPendingCallback` → 拷进 `CanFrame` → `osMessageQueuePut(..., 0, 0)`；ISR 只搬不解析，队列满就丢这一帧。
- **ADC 采集**：`HAL_ADC_Start_DMA()` 启动一次，DMA1_Channel1 循环把 CH0→`buf[0]`、CH1→`buf[1]`，任务侧零等待读取。
- **OLED 写**：一次 `MyIIC_Start()` 内连续发「地址 + 控制字 + N 字节」，靠 SSD1306 列地址自增把每字符事务数从 22 压到 4。

## 五、CAN 应用层协议（v3.0）
### 5.1 ID 设计：功能组（高 7 位）+ 节点号（低 4 位）

```text
 bit: 10 9 8 7 6 5 4 | 3 2 1 0       0x0xx 紧急/报警   0x1xx 节点上报
      └── 功能组 ─────┘└─ 节点号 ─┘   0x3xx 控制下发   0x7xx 心跳
 0x101 = GRP_PERIODIC(0x100) | NODE_A(1) = "周期数据 + 来自 A"
```

多字节一律**大端**（`PROTO_PutU16`/`PROTO_GetU16` 手动移位，不做指针强转）；**DLC 固定**，接收方先比 `frame.len` 再解析；节点号 0 保留给广播。

### 5.2 本仓库收发的帧

| ID | DLC | 方向 | 名称 | 本仓库行为 |
|---|---|---|---|---|
| `0x101` | 5 | A → 全体 | A_SENSOR | 每 100 ms 发一帧（受采样开关控制） |
| `0x701` | 2 | A → 全体 | A_HEARTBEAT | 每 1 s 发一帧 |
| `0x000` | 1 | 广播 → 全体 | ESTOP | 收到后 OLED 显示/清除 `ESTOP`，**不改变任何输出**；B0 = 1 急停 / 2 解除，未知值按急停处理 |
| `0x103` | 4 | 网关 → 全体 | GW_STATUS | 收到后 OLED 显示 WiFi/MQTT 状态与缓存条数 |
| `0x702` / `0x703` | 2 | B / 网关 → 全体 | B/GW_HEARTBEAT | 判活，3 s 超时显示 `b OFF` / `gw OFF` |
| `0x301` / `0x311` / `0x011` | 3 | 网关 → A / A → 网关 | CTRL_MOTOR_A / CTRL_TARGET_A / ALARM_A | **均未使用**：控制帧未处理，报警帧未发送（后续阶段） |

### 5.3 字节布局

| 帧 | 字节布局 |
|---|---|
| `0x101` | B0-B1 红外 ADC（大端 0~4095，越大越无遮挡）/ B2-B3 光敏 ADC（大端，越大越暗）/ B4 seq（每帧 +1，0~255 回绕） |
| `0x701` | B0 魔数 `0xA5`（`HB_MAGIC_A`）/ B1 计数（每帧 +1） |
| `0x103` | B0 WiFi / B1 MQTT / B2-B3 本地缓存条数（大端） |

### 5.4 判活、丢帧与幂等
- **判活**：ID 对、DLC 对、魔数对，三者齐才算有效心跳；用 `osKernelGetTickCount()` 记时刻，`(now - last) > 3000` 判掉线。无符号减法让 tick 回绕也算得对，靠 `online` 标志只报一次。
- **丢帧**：`seq` 差值即证据，`PROTO_SeqLost(new, old) = (new - old - 1) & 0xFF`，靠 `uint8_t` 截断自动处理 255→0 回绕，不用写分支。**本仓库只发 seq、不做统计**，统计能力留在协议层（后续阶段）。
- **幂等**：`0x301`/`0x311` 的 B2 是命令序号 `cid`，用于丢弃 CAN 自动重发造成的重复命令。本仓库尚未接收这两帧，故 `cid` 暂未使用。

## 六、硬件与引脚
### 6.1 MCU 与时钟

| 项 | 值 | 依据 |
|---|---|---|
| MCU | STM32F103**xB**（Cortex-M3，128 KB Flash / 20 KB RAM） | `stm32f103xb.h`；无法区分 C8T6/CBT6 |
| HSE | 8 MHz 外部晶振（HSE 外部时钟，非内部 RC） | `HSE_VALUE=8000000`、`RCC_HSE_ON`、`PLLSOURCE_HSE` |
| SYSCLK | 72 MHz（8 MHz × 9），Flash 2 WS | `PLLMUL = RCC_PLL_MUL9`、`FLASH_LATENCY_2` |
| HCLK | **36 MHz**（AHB = SYSCLK/2） | `AHBCLKDivider = RCC_SYSCLK_DIV2` |
| PCLK1 / PCLK2 | 36 MHz / 36 MHz（均不分频） | `APB1CLKDivider = APB2CLKDivider = DIV1` |
| ADCCLK | 6 MHz（36 MHz / 6，≤14 MHz 上限） | `RCC_ADCPCLK2_DIV6` |
| FreeRTOS | `configTICK_RATE_HZ=1000`，`configCPU_CLOCK_HZ=SystemCoreClock`（=36 MHz） | `FreeRTOSConfig.h` |

> 注意：多处注释仍按 72 MHz 推算（`adc_dma.c` 与 `HAL_ADC_MspInit` 的"APB2=72 MHz"、`iic.c` 的 `CPU_FREQ_MHZ 72`、`oneWire.c` 的"假设 SYSCLK=72 MHz"、`pwm_*.c`），与 AHB 二分频后的 36 MHz 不一致，属旧工程残留。

### 6.2 引脚

| 引脚 | 模式 | 功能 | 出处 |
|---|---|---|---|
| PA0 / PA1 | 模拟输入 | ADC1_IN0 红外 / ADC1_IN1 光敏 | `adc_dma.c`、`HAL_ADC_MspInit` |
| PA2 / PA3 | 复用推挽 / 输入上拉 | USART2_TX / USART2_RX，115200 | `HAL_UART_MspInit` |
| PA8 | 推挽输出（初值高） | 预留 LED / 被注释的 TIM1_CH1 PWM；`Error_Handler` 里翻转 | `gpio.c`、`main.c` |
| PA11 / PA12 | 输入 / 复用推挽 | CAN1_RX / CAN1_TX | `HAL_CAN_MspInit` |
| PB6 / PB7 | 开漏输出 | 软件 I2C SCL / SDA（OLED） | `iic.c` |
| PB12 | EXTI 下降沿 + 上拉 | 按键，翻转采样开关 | `interrupt.c` |
| PB15 / PC13 | 推挽输出 | PB15 板载 LED（置高后未再操作）/ PC13 心跳闪灯，2 s 翻转 | `gpio.c`、`freertos.c` |
| PC15 | 输出↔输入切换 | DHT22 单总线数据（未初始化、未调用） | `oneWire.c` |

保留未用：PA4/PA5/PA6/PA7（SPI1 CS/SCK/MISO/MOSI）、PB10/PB11（I2C2 SCL/SDA）仅在注释代码里；`GPIOD` 时钟被使能但没有用到任何 PD 引脚。

### 6.3 外设资源占用

| 外设 | 配置 | 用途 |
|---|---|---|
| ADC1 | 12 bit，扫描 + 连续 + 软件触发，CH0/CH1，采样 55.5 周期 | 单通道约 68 ADCCLK ≈ 11.3 µs，双通道一轮 ≈ 22.7 µs（每通道约 44 kSPS） |
| DMA1_Channel1 | 外设→内存，半字，内存递增，循环，高优先级 | 把 ADC 结果搬进 `adc_dma_buf[2]` |
| CAN1 | 正常模式，250 kbps（PSC 9 / BS1 13TQ / BS2 2TQ / SJW 2TQ，共 16TQ，采样点 87.5%），3 个发送邮箱，FIFO0 中断，过滤器 Bank0 掩码全 0（全通过） | 收发全部业务帧 |
| USART2 | 115200 8N1，阻塞收发，无 DMA、无环形缓冲 | 调试文本与 `0xAA55` 自定义帧 |
| TIM1 / TIM2 / SPI1 / I2C2 | 仅存在于注释代码（PWM / 捕获 / ETR / W25Q64 / MPU6050） | 未启用 |

### 6.4 中断与优先级

| 中断 | 抢占优先级 | 处理内容 |
|---|---|---|
| `USB_LP_CAN1_RX0_IRQn` | 5 | 取 FIFO0 → `CanFrame` → 入队 |
| `EXTI15_10_IRQn` | 5 | PB12 消抖 + 翻转 `g_sample_enable` |
| `TIM4_IRQn` | 15 | HAL 1 ms tick |
| `PendSV` | 15 | FreeRTOS 上下文切换 |

`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`，CAN 与 EXTI 取 5 是"仍可调用 `FromISR` API"的最低允许值；再往上调（数值更小）会破坏内核临界区，`configASSERT` 直接卡死。

## 七、构建与烧录
**现状**：仓库只有 `Core/Inc` 与 `Core/Src`，**没有** `.ioc`、`Drivers/`、`Makefile`/`CMakeLists.txt`、`.ld` 链接脚本、`.uvprojx`；单一提交 `1.0`。所以 clone 下来不能直接编译，必须先补回工程骨架。

**路径 A（推荐）CubeMX 重建工程**：新建 `STM32F103C8T6`（或同容量 `xB`）工程，Toolchain 选 STM32CubeIDE / Makefile，按下表配置（这些项必须与代码一致，否则跑不起来），生成后把本仓库 `Core/Inc`、`Core/Src` 覆盖过去，保留 `Drivers/`、启动文件与链接脚本。

| 项目 | 设置 |
|---|---|
| RCC / SYS | HSE 晶体 8 MHz；PLL ×9 → 72 MHz；**AHB /2**、APB1 /1、APB2 /1；Flash latency 2；Timebase = **TIM4**（不是 SysTick）；Debug = Serial Wire |
| FreeRTOS | CMSIS_V2；heap 8192、tick 1000、MAX_PRIORITIES 56；按 4.1/4.2 建 5 任务 2 队列 |
| ADC1 | IN0/IN1，Scan + Continuous，软件触发，55.5 周期，DMA1_Ch1 Circular / HalfWord |
| CAN1 | 250 kbps（PSC 9 / BS1 13TQ / BS2 2TQ / SJW 2TQ），Normal，ABOM=ENABLE，RX0 中断 |
| USART2 | 115200 8N1 |
| GPIO | PB15/PA8/PC13 输出，PB12 EXTI 下降沿上拉，PB6/PB7 开漏，PC15 双向 |

**路径 B 覆盖到同型号空工程**：目标工程只要是 `STM32F103xB` + HAL + FreeRTOS(CMSIS V2) + 相同外设，直接覆盖 `Core/` 即可；8 个整文件注释的 `.c` 是空翻译单元，不影响编译。**编译与烧录**：CubeIDE 直接 Build 后用 ST-Link SWD 下载；命令行可 `make -j` 后 `st-flash write build/*.bin 0x8000000`。Keil 需自建工程并手动加入 HAL、CMSIS、FreeRTOS 与 `startup_stm32f103xb.s`（本仓库未提供）。`HAL_MspInit()` 里是 `SWJ_NOJTAG`，**SWD 可用、JTAG 不可用**。

**首上电验收**：① PC13 每 2 s 翻转（调度器在跑）；② 不按 PB12 时总线上只有 `0x701`；③ 按一下 PB12 后出现 `0x101`（DLC=5、100 ms 一帧）；④ 接上 B 与网关后 OLED 第 4 行出现 `b ON`/`gw ON`，对端断电 3 s 后变 `b OFF`/`gw OFF`；⑤ 串口 115200 无输出——这是现状，`serialSendTask` 的数据源被注释掉了。

## 八、技术栈与技术亮点

| 层次 | 选型 |
|---|---|
| MCU / RTOS | STM32F103xB（72 MHz SYSCLK / 36 MHz HCLK）+ FreeRTOS 10.3.1（heap_4，8 KB）+ CMSIS-RTOS2 接口层 |
| 驱动 / 总线 | STM32CubeF1 HAL（RCC/GPIO/ADC/DMA/CAN/UART/TIM）；CAN 2.0A 250 kbps |
| 协议 / 显示 | `protocol.h` v3.0（7+4 位 ID、大端、固定 DLC、seq/cid/魔数）；SSD1306 128×64 软件 I2C |

- **协议层零依赖**：`protocol.h` 只 include `<stdint.h>`，全部 `static inline`，没有 `.c` 也没有全局变量——STM32 与 ESP32 复制同一份就能对上。
- **ADC 零拷贝**：扫描+连续+DMA 循环，业务侧只读内存数组，采样节奏与任务节奏解耦；CAN RX 回调只把整帧拷进结构体再入队（`can.h` 注释说明了为什么不用一组 `volatile` 全局变量：会拼出两帧混合数据），`osMessageQueuePut` 超时给 0，绝不阻塞中断。
- **判活回绕安全 + 丢帧可观测**：无符号 tick 减法让回绕也成立；心跳带魔数做 ID+DLC+魔数三重校验；`0x101` 的 `seq` 是应用层唯一的丢帧证据，配 `PROTO_SeqLost()` 一行算出缺口。
- **8 KB 堆上跑 5 任务**：每个 `osThreadNew` 都判空，堆耗尽时进 `Error_Handler()`，而不是让某个任务静默不跑；I2C 事务也从 22 次/字符压到 4 次（见 9.2）。

## 九、典型问题与排查
### 9.1 上电只有心跳，一条 `0x101` 都没有
- **现象**：CAN 分析仪上每秒一帧 `0x701`，网关显示 A 在线，但收不到传感器数据。
- **根因**：`g_sample_enable` 初值为 0，`photoAdcTask` 每 100 ms 只在 `==1` 时才采样发送；开关靠 PB12 按键中断翻转。
- **定位手法**：看 `freertos.c` 里 `if(g_sample_enable == 1)` 这一层；注意 `serialSendTask` 的数据源 `osMessageQueuePut` 已被注释，串口不会打出任何东西，别指望日志。
- **处理**：按一下 PB12。若希望上电即采，把初值改 1，或由网关下发使能（后续阶段）。

### 9.2 OLED 每字符 22 次 I2C 事务，刷屏 >100 ms，接收队列被灌满丢帧
- **现象**：丢帧数稳定在十几帧、且"刚上电一段时间不丢、之后稳定丢"，同时 OLED 刷新明显卡顿。
- **根因**：旧版 `OLED_ShowChar` 逐字节写——上下两页各「3 次光标命令 + 8 次单字节数据」= **22 次完整 Start/Stop**；软件 I2C 全是忙等，一屏约 20 字符 ≈ 440 次事务 ≈ 100 ms+，与 100 ms 的帧到达周期同量级。消费速度一旦跟不上，深度 8 的 `canRxQueue` 就满，中断里 `osMessageQueuePut` 超时为 0，**整帧静默丢弃**。
- **定位手法**：数事务数（比数时间可靠）；用 `seq` 缺口判断丢帧；累计 `osMessageQueuePut` 的返回值（当前被 `(void)` 扔掉，丢帧无声）。
- **处理**：`oled.c` 已改为 `OLED_WriteCmdSeq` / `OLED_WriteDataSeq` 批量写，每字符 22 → 4 次事务。治本做法是接收路径不刷屏：接收任务只存值置标志，另开低优先级任务刷"最新值"。A 目前只在 `canRxTask` 刷状态行，所以这个坑暂时不发作；但 `OLED_Update()` 仍是逐字节 1024 次事务，一旦整屏刷新就会原样复发。

### 9.3 能收不能发：TEC 一路涨、REC=0、LEC=4
- **现象**：接收一切正常，自己发的帧几乎没人 ACK，TEC 涨到 255 后 bus-off，对端显示离线。
- **根因**（物理层）：CAN 收发器接收侧几乎不耗电，发送侧要给 60 Ω 终端灌几十 mA。供电通路压降大时，显性位一来收发器 VCC 就塌，差分电平不到位 → 对端解不出来 → 不 ACK → 每帧 TEC +8。**LEC=4（位隐性错）说明自己发显性、回读隐性**，问题在本节点发送侧；LEC=3 才是"帧没问题、只是没人 ACK"。
- **定位手法**：读 `CAN1->ESR`：TEC=`bits23:16`、REC=`bits31:24`、LEC=`bits6:4`。本仓库没有这段自检代码，需临时加一个 OLED 自检屏。
- **处理**：换短而硬的 5 V 供电（ST-Link 直接供 5 V 即可）；`can.c` 里 `AutoBusOff`/`AutoRetransmission` 已设为 `ENABLE`，物理层恢复后能自动从 bus-off 爬回来（注释写明：`DISABLE` 时硬件置 `INIT` 而工程里无人清 `INIT`，掉线只能手动复位）。CAN 必须外接收发器（TJA1050 等），总线两端各一个 120 Ω，缺了就一直报错。

### 9.4 只有 3 个发送邮箱，满了静默丢帧；且两个任务并发调 `CAN_Send()`
- **现象**：偶发丢帧，`seq` 有缺口，但 TEC/REC 干净（说明不是电气问题）。
- **根因**：`CAN_Send()` 在 `HAL_CAN_GetTxMailboxesFreeLevel() == 0` 时直接 `return 0` 丢帧，调用方没检查返回值；而 `photoAdcTask`(32) 与 `canHeartTask`(15) 都直接调它，"查空闲邮箱 → 写邮箱"之间没有互斥，高优先级任务可在中间抢占。
- **定位手法**：统计 `CAN_Send()` 返回值；观察缺口是否只在两帧发送时刻接近时出现。
- **处理**：把发送收敛到单一任务，或给 `CAN_Send()` 加互斥量（后续阶段）。

## 十、相关仓库与备注

| 仓库 | 地址 | 关系 |
|---|---|---|
| control_node | https://github.com/YumK00/control_node | 同一条 CAN 总线上的执行节点 B，收本节点广播的 `0x101`，上报 `0x102`/`0x702` |
| gateway_node | https://github.com/YumK00/gateway_node | 同一条总线上的 ESP32-S3 网关，把 `0x101` 转成 MQTT 上行；下发的 `0x301`/`0x311` 本节点尚未解析 |
| v_car | https://github.com/YumK00/v_car | 独立项目：车载域 T-Box 仿真，不与本链路共用 Broker 或协议 |
| weather_bureau | https://github.com/YumK00/weather_bureau | 独立项目：气象数据开放平台（Linux C++ 后端） |

**空壳与未接入**：① 整文件被注释（不参与编译）—— `adc.c/h`、`i2c.c/h`(MPU6050)、`spi.c/h`(软件 SPI)、`spii.c/h`(SPI1+W25Q64)、`pwm_ic.c/h`(TIM2_CH2 输入捕获测频)、`pwm_oc.c/h`(TIM1_CH1 呼吸灯)、`timer_extern_interrupt.c/h`(TIM2 ETR 计数)、`timer_intern_interrupt.c/h`(TIM2 1 s 中断)，它们与 `adc_dma.c` 存在 PA0/PA1/PA8 引脚重叠，恢复前要先解冲突。② `oneWire.c`/`DHT22.c` 是真代码但**无调用者**，且只有起停 + 5 字节读取，缺 40 bit 解析与校验和；`Delay_us()` 按 72 MHz 标定，实际 36 MHz 会偏慢约一倍。③ `serialSendTask` 的生产者被注释，串口输出与 OLED 数值刷新**实际不工作**，该任务永久阻塞在空队列上，白占 1 KB 栈。

**已知问题**：`Error_Handler()` 先 `__disable_irq()` 再 `HAL_Delay(200)`——HAL 时基是 TIM4 中断，中断一关 `uwTick` 不再增长，`HAL_Delay` 永远等下去，PA8 根本不会翻转，想闪灯报错反而彻底静默；`iic.c` 的 `CPU_FREQ_MHZ 72` 与实际主频不符，软件 I2C 每位实际约 2 µs，OLED 事务耗时翻倍；`OLED_Init()` 注释里的"PB8-SCL PB9-SDA"与实际 PB6/PB7 不符；`MX_GPIO_Init()` 只开 GPIOC/GPIOD/GPIOA 时钟，GPIOB 靠 `LED_Init()`/`Key_Init()`/`MyIIC_Init()` 各自补开，初始化顺序一变 PB 外设就静默失效，且 `GPIOD` 时钟使能后未用。

**后续阶段 / 规划中**：`0x011 ALARM_A` 报警上报（`ALM_SENSOR_FAULT`/`ALM_THRESH_OVER` 已定义未使用）；接收并执行 `0x301 CTRL_MOTOR_A` / `0x311 CTRL_TARGET_A` 并用 `cid` 做幂等；`PROTO_SeqLost()` 丢帧统计与队列 Put 失败计数；`0x000` 急停的实际安全动作（目前只刷 OLED，未停采集、未联动执行器）；UDS 诊断 `0x7E0`/`0x7E8`（协议表标为阶段 2/4）、DHT22 温度接入、SPI Flash 本地缓存。
