# 文件清单 + 重构勾选

最后更新: 2026-09-01 Tuesday

---

## 📂 所有 .c / .h 源码清单

按目录分组。每改一个,在 `[ ]` 里打 `[x]`。

### Drivers/

- [x] `Drivers\w25q64.c`
- [x] `Drivers\w25q64.h`
- [x] `Drivers\chinese.c`
- [x] `Drivers\chinese.h`
- [x] `Drivers\board_pins.h`
- [x] `Drivers\spi_bus.h`
- [x] `Drivers\touch.c`
- [x] `Drivers\touch.h`
- [x] `Drivers\lcd.c`
- [x] `Drivers\lcd.h`
- [x] `Drivers\tf.c`
- [x] `Drivers\tf.h`

### ui/ (UI 框架)

- [x] `ui\ui_btn.c`
- [x] `ui\ui_btn.h`
- [x] `ui\ui_core.c`
- [x] `ui\ui_core.h`
- [x] `ui\ui_geometry.c`
- [x] `ui\ui_geometry.h`
- [x] `ui\ui_draw.c`
- [x] `ui\ui_draw.h`
- [x] `ui\ui_tile.c`
- [x] `ui\ui_tile.h`
- [ ] `ui\ui_dropdown.c`
- [ ] `ui\ui_dropdown.h`
- [ ] `ui\ui_file_browser.c`
- [ ] `ui\ui_file_browser.h`
- [ ] `ui\ui_input.c`
- [ ] `ui\ui_input.h`
- [ ] `ui\ui_keyboard.c`
- [ ] `ui\ui_keyboard.h`
- [ ] `ui\ui_scrollview.c`
- [ ] `ui\ui_scrollview.h`
- [ ] `ui\ui_modal.c`
- [ ] `ui\ui_modal.h`
- [ ] `ui\ui_numpad.c`
- [ ] `ui\ui_numpad.h`
- [ ] `ui\ui_packet_table.c`
- [ ] `ui\ui_packet_table.h`
- [ ] `ui\ui_progressbar.c`
- [ ] `ui\ui_progressbar.h`
- [ ] `ui\ui_slider.c`
- [ ] `ui\ui_slider.h`
- [ ] `ui\ui_switch.c`
- [ ] `ui\ui_switch.h`
- [ ] `ui\ui_tabview.c`
- [ ] `ui\ui_tabview.h`
- [ ] `ui\ui_terminal.c`
- [ ] `ui\ui_terminal.h`
- [ ] `ui\ui_waveform.c`
- [ ] `ui\ui_waveform.h`
- [ ] `ui\ui_toast.c`
- [ ] `ui\ui_toast.h`
- [ ] `ui\ui_stepper.c`
- [ ] `ui\ui_stepper.h`
- [ ] `ui\ui_checkbox_group.c`
- [ ] `ui\ui_checkbox_group.h`
- [ ] `ui\ui_hex_view.c`
- [ ] `ui\ui_hex_view.h`
- [ ] `ui\ui_drawer.c`
- [ ] `ui\ui_drawer.h`
- [ ] `ui\ui_status_bar.c`
- [ ] `ui\ui_status_bar.h`
- [ ] `ui\ui_picture.c`
- [ ] `ui\ui_picture.h`

### app/ (host 端 app + plugin 框架)

- [ ] `app\app_loader.c` ⚠️ **重构重点** (拆 4 文件)
- [ ] `app\app_loader.h`
- [ ] `app\app_base.h`
- [ ] `app\app_shared_ram.h`
- [ ] `app\app_api.h` ⚠️ **重构重点** (字段重排 + 完整性检查)
- [ ] `app\app_manager.c`
- [ ] `app\app_manager.h`
- [ ] `app\app_template.c`
- [ ] `app\app_template.h`
- [ ] `app\app_led.c`
- [ ] `app\app_led.h`
- [ ] `app\app_led.h` (重复)
- [ ] `app\app_pwm.c`
- [ ] `app\app_pwm.h`
- [ ] `app\app_adc.c`
- [ ] `app\app_adc.h`
- [ ] `app\app_audio.c`
- [ ] `app\app_audio.h`
- [ ] `app\app_i2c_spi.c`
- [ ] `app\app_i2c_spi.h`
- [ ] `app\app_pinout.c`
- [ ] `app\app_pinout.h`
- [ ] `app\app_sd_msc.c`
- [ ] `app\app_sd_msc.h`
- [ ] `app\app_swd_probe.c`
- [ ] `app\app_swd_probe.h`
- [ ] `app\app_uart_term.c`
- [ ] `app\app_uart_term.h`
- [ ] `app\cmsis_dap.c`
- [ ] `app\cmsis_dap.h`
- [ ] `app\audio_i2s.c`
- [ ] `app\audio_i2s.h`
- [ ] `app\swd_flasher.c`
- [ ] `app\swd_flasher.h`
- [ ] `app\usb_descriptors.c`
- [ ] `app\msc_disk.c`
- [ ] `app\tusb_config.h`
- [ ] `app\algo_def.h`
- [ ] `app\dog_data.h`

### fatfs/ (文件系统, 一般不动)

- [ ] `fatfs\ff15\source\ff.c`
- [ ] `fatfs\ff15\source\ff.h`
- [ ] `fatfs\ff15\source\ffconf.h`
- [ ] `fatfs\ff15\source\ffsystem.c`
- [ ] `fatfs\ff15\source\ffunicode.c`
- [ ] `fatfs\ff15\source\diskio.h`
- [ ] `fatfs\src\f_util.c`
- [ ] `fatfs\include\f_util.h`
- [ ] `fatfs\src\ff_stdio.c`
- [ ] `fatfs\include\ff_stdio.h`
- [ ] `fatfs\src\glue.c`
- [ ] `fatfs\src\my_debug.c`
- [ ] `fatfs\include\my_debug.h`
- [ ] `fatfs\src\rtc.c`
- [ ] `fatfs\include\rtc.h`
- [ ] `fatfs\include\util.h`

### plugins/ (TF 卡动态加载, 重构重点)

- [ ] `plugins\led_controller\main.c`
- [ ] `plugins\led_controller\plugin_led_controller.c`
- [ ] `plugins\led_controller\plugin_led_controller.h`
- [ ] `plugins\led_controller\CMakeLists.txt`
- [ ] `plugins\led_controller\plugin.ld`
- [ ] `plugins\novel_reader\main.c`
- [ ] `plugins\novel_reader\plugin_novel_reader.c` ⚠️ **拆 7 文件**
- [ ] `plugins\novel_reader\plugin_novel_reader.h`
- [ ] `plugins\novel_reader\CMakeLists.txt`
- [ ] `plugins\novel_reader\plugin.ld`
- [ ] `plugins\wire_quality\main.c`
- [ ] `plugins\wire_quality\plugin_wire_quality.c`
- [ ] `plugins\wire_quality\plugin_wire_quality.h`
- [ ] `plugins\wire_quality\CMakeLists.txt`
- [ ] `plugins\wire_quality\plugin.ld`
- [ ] `plugins\wire_quality\toolchain-arm.cmake`
- [ ] `plugins\wire_tester\**` ⚠️ **整个目录还不存在**

---

## 📱 app / plugin 清单

### host 端内置 app (编译进 .uf2)

- [ ] **app_template** (`app/app_template.c`) — app 框架 demo / 模板
- [ ] **app_led** (`app/app_led.c`) — LED 控制
- [ ] **app_pwm** (`app/app_pwm.c`) — PWM 输出
- [ ] **app_adc** (`app/app_adc.c`) — ADC 采样
- [ ] **app_audio** (`app/app_audio.c` + `app/audio_i2s.c`) — I2S 音频
- [ ] **app_i2c_spi** (`app/app_i2c_spi.c`) — I2C / SPI 总线扫描
- [ ] **app_pinout** (`app/app_pinout.c`) — 排针图可视化
- [ ] **app_sd_msc** (`app/app_sd_msc.c` + `app/msc_disk.c` + `app/usb_descriptors.c`) — TF 卡 U 盘模式
- [ ] **app_swd_probe** (`app/app_swd_probe.c` + `app/swd_flasher.c`) — SWD 探测 + flash 烧写
- [ ] **app_uart_term** (`app/app_uart_term.c`) — UART 串口终端
- [ ] **cmsis_dap** (`app/cmsis_dap.c`) — CMSIS-DAP 调试协议
- [ ] **app_manager** (`app/app_manager.c`) — app 总菜单 / 调度
- [ ] **app_loader** (`app/app_loader.c`) — plugin 加载器 (32KB RAM 池 + .papp 跳转)

### TF 卡动态 plugin (`.papp` 文件)

- [ ] **led_controller** (`plugins/led_controller/`) — LED 效果控制 demo
- [ ] **novel_reader** (`plugins/novel_reader/`) — GBK/UTF-8 小说阅读器 ⚠️ **bug 多, 重构**
- [ ] **wire_quality** (`plugins/wire_quality/`) — 杜邦线质量测试 (模式 1 / 2 / 3)
- [ ] **wire_tester** (`plugins/wire_tester/`) — ⚠️ **未建, 待建**

---

## 🎯 重构优先级

### P0 (框架层, 必须先做)
- [ ] `app/app_loader.c` 拆 4 文件
- [ ] `app/app_api.h` 字段重排 + 完整性检查
- [ ] `plugins/novel_reader/plugin_novel_reader.c` 拆 7 文件
- [ ] `plugins/wire_tester/` 建目录

### P1 (功能清理)
- [ ] `plugins/novel_reader/plugin_novel_reader.c` 黑块修复
- [ ] `plugins/novel_reader/plugin_novel_reader.c` 翻页方向配置化
- [ ] `plugins/novel_reader/plugin_novel_reader.c` UTF-8→GBK 表替换
- [ ] `plugins/wire_tester/` 三模式实现

### P2 (质量)
- [ ] `plugins/led_controller/` 用 ui_btn 系统
- [ ] 所有 plugin 加 size budget 编译检查
- [ ] `.gitignore` 加 `build/` 和 `dist/`
- [ ] `CLAUDE.md` + `README.md`

---

## 📌 备忘

- 总源码文件: **~110** 个 .c/.h (不含 build/ 和 fatfs/ 内部)
- 内置 app: **13** 个
- plugin: **3** 个 + 1 个待建
- 当前 papp: `dist/led_controller.papp` / `dist/novel_reader.papp` (6164 B)