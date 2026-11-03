# Pico 多功能嵌入式工具平台

这是一个基于 Raspberry Pi Pico / RP2040 的多功能嵌入式工具固件。项目把 LCD 触摸界面、常用总线工具、CMSIS-DAP/SWD、USB 复合设备、脚本执行和可插拔 PAPP 应用整合到同一套固件中，目标是做成一块可独立使用的开发、调试和测试工具板。

## 项目现状

当前主固件由 `pico.cpp` 启动，并通过 `app_manager` 注册和切换内置 App。当前源码中实际注册的主要功能包括：

- 系统设置
- TF 卡 / USB MSC 文件功能
- I²C 工具
- SPI 工具
- 引脚排列与功能查看
- 动态脚本工具
- ESP32-C3 通信工具
- UART 串口终端
- DAP / CMSIS-DAP 调试工具
- Game Boy 模拟器
- 宠物 / 桌面小组件

源码中还保留了 ADC、PWM、音频、红外、HID 键鼠、旧版 SWD 探测等模块；它们是否出现在主界面由 `pico.cpp` 中的 `app_manager_register()` 注释状态决定。

## 主要目录

```text
.
├─ pico.cpp              # 固件入口、硬件初始化、主循环和 App 注册
├─ config.h / config.c   # 固件版本、板级全局配置
├─ CMakeLists.txt        # 主工程构建入口
├─ Drivers/              # LCD、触摸、W25Q64、TF 卡、中文字体等底层驱动
├─ ui/                   # 轻量级 LCD UI 组件库
├─ app/                  # 编译进主固件的内置 App 和应用管理器
├─ fun/                  # UART、I²C、SPI、SWD、PIO、脚本等功能逻辑
├─ pio/                  # PIO 程序和 PIO 资源管理器
├─ fatfs/                # FatFs 文件系统及适配层
├─ plugins/              # 独立 PAPP 动态应用
├─ tools/                # 算法转换、PAPP 生成和辅助工具
├─ flash_pico.py         # Pico 固件烧录辅助脚本
└─ TODO.md               # 开发计划和重构记录
```

## 软件分层

### 1. 固件入口层

`pico.cpp` 负责：

- 初始化 Pico SDK、USB、LCD、触摸、ADC 温度传感器和系统 UI；
- 创建全局 `app_manager_t`；
- 注册内置 App；
- 运行主循环、处理触摸事件、更新状态栏和绘制界面；
- 处理系统 LED、亮度和进入 BOOTSEL 等全局操作。

### 2. UI 组件层

`ui/` 提供面向小型嵌入式屏幕的组件，包括：

- 几何和绘制：`ui_geometry`、`ui_draw`、`ui_tile`、`ui_core`；
- 基础控件：按钮、滑块、开关、步进器、复选框组；
- 页面控件：TabView、ScrollView、Drawer、Modal、Toast；
- 数据显示：终端、波形、十六进制视图、数据包表、进度条；
- 输入和文件：输入框、键盘、数字键盘、文件浏览器；
- 辅助控件：状态栏、图片、下拉框等。

内置 App 通过 `app_base.h` / `app_manager.h` 约定生命周期，通常包含初始化、更新、触摸处理和绘制几个阶段。脏矩形机制用于减少不必要的整屏重绘。

### 3. 功能逻辑层

`fun/` 将具体硬件操作从 UI 中分离出来：

- `fun_uart.*`：硬件 UART 和 USB 串口相关逻辑；
- `fun_pio_uart.*`：PIO UART；
- `fun_i2c.*`：GPIO 开漏模拟 I²C，默认 GP4/GP5；
- `fun_spi.*`：SPI 收发；
- `fun_swd.*`、`fun_pio_swd.*`：软件 SWD 与 PIO SWD；
- `fun_esp.*`：与 ESP32-C3 的串口通信；
- `pin_manager.*`：功能引脚占用和资源管理；
- `fun_script_engine.*`：脚本原语执行；
- `thumb_disasm.*`、`sym_loader.*`：Thumb 反汇编和符号辅助；
- `fun_task_flow.*`：流程任务和步骤执行。

### 4. PIO 层

`pio/` 中包含 PIO 状态机资源管理器和 PIO 程序，例如 UART、SWD、红外等。`pio_manager` 负责程序加载、状态机分配和资源释放；具体功能模块通过它申请 PIO 资源，而不是直接互相抢占状态机。

### 5. 内置 App 层

`app/` 中的 App 会被编译进主固件。典型模块包括：

- `app_uart.c` / `app_uart_term.c`：UART 配置、终端、USB 与外部串口透传；
- `app_i2c.c`、`app_spi.c`：总线配置和收发界面；
- `app_dap.c`、`cmsis_dap.c`：CMSIS-DAP 协议、SWD 探测和调试交互；
- `swd_flasher.c`：基于 SWD 的目标芯片烧录和调试访问；
- `app_sd_msc.c`、`msc_disk.c`、`usb_descriptors.c`：TF 卡 USB MSC/U 盘功能；
- `app_script.c`：文本脚本编辑、执行和 SWD 等原语调用；
- `app_loader.c`：从存储介质加载并运行独立 PAPP。

CMSIS-DAP 当前保留软件 SWD 后端作为回退路径，PIO SWD 相关代码位于 `fun/` 和 `pio/`，实际是否接管由后端初始化和运行状态决定。

## PAPP 插件系统

`plugins/` 中的应用不是直接链接进主固件，而是编译成独立的 `.papp` 二进制。插件通过 `app/app_api.h` 获取主系统提供的能力，例如：

- LCD 绘制和 UI 按钮；
- 时间、延时和 GPIO；
- 日志输出、请求重绘和退出；
- FatFs 文件访问；
- OLED 文本写入和清屏桥接。

插件入口由 `main.c` 导出，运行时由 `app_loader.c` 搬运到专用 RAM 区执行。链接脚本 `plugin.ld` 将插件映射到 `APP_RAM_BASE`，当前插件运行池为 32 KB。插件退出后，加载器会停止 Core 1 工作协程并清理插件 RAM。

仓库中已有的独立插件包括：

- `wire_quality`：杜邦线/连线质量测试；
- `novel_reader`：文本阅读器；
- `snake`、`dino_runner`、`gomoku`、`xiangqi`、`2048`：小游戏；
- `led_controller`：LED 控制示例；
- `oled_text`、`oled_display`：OLED 相关实验；
- `reflex`：反应测试。

插件能否被当前构建直接生成，以各目录中的 `CMakeLists.txt` 以及根目录是否 `add_subdirectory()` 为准。不要仅根据 `plugins/` 目录存在就认为该插件已经出现在设备菜单中。

## USB 与存储

主工程使用 TinyUSB/Pico SDK 的 USB 能力，并由 `app/usb_descriptors.c` 管理复合设备描述符。项目围绕以下 USB 功能组织：

- USB CDC：系统日志、终端和串口辅助通道；
- USB HID：CMSIS-DAP 等调试接口；
- USB MSC：将 TF 卡等存储介质呈现为 U 盘；
- 1200 baud BOOTSEL：通过 USB CDC 配置支持进入 RP2040 BOOTSEL。

文件和应用资源主要使用 FatFs；`W25Q64` 驱动用于板载 SPI Flash，TF 卡由 `Drivers/tf.c` 和 `app_sd_msc.c` 协作管理。

## 硬件连接摘要

常用固定引脚可在 `Drivers/board_pins.h` 和功能模块头文件中查看。当前代码中常见连接包括：

| 功能 | 引脚 |
|---|---|
| SWD | GP2 = SWCLK，GP3 = SWDIO |
| I²C 工具 / OLED 示例 | GP4 = SDA，GP5 = SCL |
| SPI 工具默认引脚 | GP4/GP5/GP6/GP7 |
| 逻辑分析仪 | CH0~CH3 = GP4~GP7，电平触发，PIO 采样 + DMA（GP2/GP3 保留给 SWD） |
| ESP32-C3 UART | 由 `fun_esp` 和 UART 配置决定 |
| LCD / TF / W25Q64 | 见 `Drivers/board_pins.h` |

如果同时启用多个功能，应优先通过 `pin_manager` 或 App 的引脚配置界面检查资源占用，避免不同外设复用同一 GPIO。

### 逻辑分析仪（电平触发）

两种用法，共用同一套 PIO/DMA 采样设计：

**① USB 网页上位机（与 Tang Nano 9K 版同构）**

- 固件协议引擎在 `app/la_usb.c`，帧协议与 TangNano 版一致（`A5/5A + CRC8`），命令号 0x01~0x08 相同，另新增 `0x0A SET_RLE`；IDENT 回 `"PICO"`，固件版本号 **3**（≥3 表示支持 RLE）。
- 通道：CH0~CH3 = GP4~GP7，共 4 通道（GP2/GP3 保留给 DAPLink SWD）。
- 上位机：`web/index.html` 单文件，Web Serial 直连 Pico 的 CDC 串口（波特率随意，CDC 忽略），桌面版 Chrome/Edge 打开。
- 深度 32768 点、采样率 10 kS/s ~ 120 MS/s（240MHz / 分频档）、连续乒乓滚动模式。20 MHz 以上为实验档：高频下 DMA/FIFO 可能来不及搬运而丢点，且 32768 点覆盖的窗口很短，建议先用 ≤20 MHz 验证。
- **触发是 PIO `WAIT GPIO` 硬件电平触发**，触发点固定为第 0 点（全触发后采集）；上升/下降沿按"等待变高/变低"近似。
- **RLE 压缩**：上位机勾选后经 `0x0A` 开关，固件在 DMA 半区完成后原地压缩（值取低 4bit；`0x00~0x7F` 为字面样本，`0x80~0xFF` 为游程：低 7bit = 重复数-1，后跟 1 字节样本值；压缩后长度 ≤ 原始长度，恒定电平可达 ~64×）。单次采集读回压缩数据由上位机展开；连续模式下限速约 1 MHz（RLE 开）/ 300 kHz（关）。未开启 RLE 时为原始字节流，仅使用 bit0~3。
- LA 引擎懒启动：第一条合法命令到达才申请 PIO/DMA；非采集中 60 秒无命令自动释放。**引擎活跃期间整个 plugin RAM 池被用作 32KB 采样缓冲，`app_loader` 会拒绝加载插件**，释放后恢复。

**② 屏幕触控 App**

实现位于 `app/app_la.c`，**当前已在 `pico.cpp` 中取消注册**（代码保留，恢复 `app_manager_register()` 即可重新启用）。网页上位机在解码、缩放和采集体验上更完整，日常推荐使用上位机。

- **通道**：CH0~CH5 = GP2~GP7，6 通道数字电平，输入请勿超过 3.3V。注意与 4 通道上位机版引脚布局不一致，同时启用会与 SWD 冲突。
- **触发**：点 `T:` 按钮循环切换「立即 / CH0 高 / CH0 低 / … / CH5 低」。选择电平触发后按「开始采集」，App 会轮询等待该通道达到目标电平，命中瞬间启动一帧 DMA，波形左边缘即触发时刻；5 秒未命中提示超时。触发为软件轮询，适合持续电平，很窄的脉冲可能漏触发。
- **采样率**：`Fs:` 按钮循环 10k / 50k / 100k / 250k / 500k / 1M / 2M / 5M / 10M / 20M S/s，基于 240MHz 系统时钟经 PIO 分频；每次固定采集 4096 点。
- **波形**：`Z:` 切换每像素采样点（1/2/4/8）；手指拖动波形平移时间轴，A/B 双游标按当前采样率换算 dT。
- **自检**：用杜邦线把某个通道接 3.3V 或 GND，选「立即」采集即可看到恒定高/低电平；状态条实时显示 D0~D5 当前电平，可用来确认接线是否正确。

两种用法的采样实现都依赖一个 PIO 特性：**PIO RX FIFO 只能按 32bit 访问**，因此 PIO 程序用 `IN PINS,8 ×4 + autopush(32)`——一个 word 装 4 个样本，右移进 ISR 使字节按时间顺序排列，上位机读取零解包（上位机原始模式仅使用 bit0~3 = CH0~CH3）。

## 构建环境

项目使用 CMake + Ninja + Raspberry Pi Pico SDK。当前工程配置针对：

- RP2040 / Raspberry Pi Pico；
- Pico SDK 2.2.0；
- ARM GNU Embedded Toolchain；
- C11 / C++ 编译；
- PIO、DMA、SPI、I²C、UART、ADC、PWM 和 USB SDK 库。

典型构建流程：

```powershell
# 在项目根目录执行
cmake -S . -B build -G Ninja
cmake --build build
```

构建主固件后，`build/` 中会生成 `.elf`、`.bin`、`.hex`、`.uf2` 等产物。独立插件通常会在其构建目录中生成同名 `.papp` 文件。

如果使用项目配置的 Pico SDK/CMake 路径，也可以直接使用仓库中已经生成的 `build` 目录重新编译。

## 烧录与调试

- RP2040 固件：使用生成的 `.uf2` 拷贝到 Pico 的 BOOTSEL U 盘，或使用 `flash_pico.py` 辅助脚本；
- SWD/CMSIS-DAP：默认连接 GP2/GP3，具体目标复位、探测和烧录逻辑位于 `cmsis_dap.c`、`swd_flasher.c` 和 `fun_swd`；
- PAPP：将 `.papp` 文件复制到设备可访问的应用目录，再通过 `app_loader` 加载；
- 脚本：通过脚本 App 加载文本脚本，调用 `fun_script_engine` 暴露的原语。

逻辑分析仪已实现（`app/app_la.c`，详见下方「逻辑分析仪」小节）；PIO 高速传输、不同目标芯片的 flash size 识别等功能仍需结合具体目标板和接线进行板上验证，不能仅以能通过编译作为硬件功能已经验证的依据。

## 开发约定

1. UI 只负责显示和交互，硬件事务尽量放到 `fun/` 或对应底层驱动。
2. 内置 App 通过 `app_manager` 注册，不要在 App 之间直接互相调用内部状态。
3. PAPP 只能通过 `pico_host_api_t` 使用主系统服务，并注意 32 KB 运行 RAM 限制。
4. 新增 `pico_host_api_t` 字段时追加到结构体末尾，保持旧版 PAPP 的 ABI 偏移兼容。
5. PIO、GPIO、DMA、USB 和显示 DMA 都是共享资源，修改前先检查资源生命周期和退出路径。
6. 修改构建文件后重新配置 CMake，避免 IntelliSense 或 Ninja 继续使用旧的生成信息。
7. 构建通过不等于硬件验证完成，串口、SWD、OLED、TF 卡和 USB 模式都应分别实测。

## 后续方向

根据当前代码结构，后续适合继续推进的方向包括：

- 将引脚配置、资源占用和各 App 配置统一到共享配置模型；
- 完善 PIO UART/SWD 与软件后端的稳定切换和错误恢复；
- 将 CMSIS-DAP 调试操作进一步拆分为探测、运行控制、寄存器、内存和烧录层；
- 继续降低 UI 全屏重绘和阻塞式外设访问；
- 完善 PAPP 的安装、版本、资源和错误报告机制；
- 为不同 Cortex-M/RISC-V 目标增加可配置的探测和烧录描述。

