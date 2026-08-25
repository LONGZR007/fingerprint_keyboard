# Tasks

- [x] Task 1: 创建工程目录结构并复制 SDK / BLE / CLI 源码
  - [x] SubTask 1.1: 创建 `/workspace/combo_keyboard/` 目录及子目录 `APP/include`、`APP`、`Profile`、`HAL`、`LIB`、`Ld`、`RVMSIS`、`Startup`、`StdPeriphDriver`
  - [x] SubTask 1.2: 复制 `/workspace/sdk/CH583EVT/EVT/EXAM/SRC` 下的 `Ld/`、`RVMSIS/`、`Startup/`、`StdPeriphDriver/` 到新工程
  - [x] SubTask 1.3: 复制 BLE HID_Keyboard 的 `APP/`（cli.c, cli_app_cmds.c, cli_os_compat.c, cli_uart.c, hidkbd.c, hidkbd_main.c, include/）到新工程 `APP/`
  - [x] SubTask 1.4: 复制 BLE HID_Keyboard 的 `Profile/`、`HAL/`（从 `sdk/CH583EVT/EVT/EXAM/BLE/HAL`）、`LIB/`（从 `sdk/CH583EVT/EVT/EXAM/BLE/LIB`）到新工程
  - [x] SubTask 1.5: 复制 `wrappers/`（riscv-wch-elf-gcc 等）和 `makefile.defs` 模板

- [x] Task 2: 实现 USB 复合设备描述符和初始化（`usb_composite.c` + `usb_composite.h`）
  - [x] SubTask 2.1: 编写复合设备描述符（class=0xEF/0x02/0x01，IAD），配置描述符含 3 个接口（HID 键盘 + CDC 通信 + CDC 数据）
  - [x] SubTask 2.2: 编写 HID 键盘报告描述符（复用 CompoundDev 的 KeyRepDesc）
  - [x] SubTask 2.3: 编写 CDC 功能描述符（Header / ACM / Union / Call Management）
  - [x] SubTask 2.4: 实现端点初始化：EP1=HID IN, EP2=CDC 通知 IN, EP3=CDC 数据 IN, EP4=CDC 数据 OUT
  - [x] SubTask 2.5: 实现 `USB_IRQHandler` 中断处理（Setup 包 + 端点传输标志保存）
  - [x] SubTask 2.6: 实现主循环 USB 处理函数 `usb_composite_task()`（延迟处理 Setup 请求和端点数据）

- [x] Task 3: 实现 CDC 类请求处理和数据收发 API
  - [x] SubTask 3.1: 实现 Setup 请求分发：Get Descriptor（设备/配置/字符串/HID 报告）、Set Address、Set Configuration
  - [x] SubTask 3.2: 实现 CDC 类请求：SET_LINE_CODING、GET_LINE_CODING、SET_CONTROL_LINE_STATE
  - [x] SubTask 3.3: 实现 CDC 数据发送 `usb_cdc_send(buf, len)` 通过 EP3 IN Bulk
  - [x] SubTask 3.4: 实现 CDC 数据接收缓冲和 `usb_cdc_recv(buf, maxlen)` 从 EP4 OUT

- [x] Task 4: 实现 USB HID 键盘发送 API
  - [x] SubTask 4.1: 实现 `usb_hid_send_key(modifier, keycode)` 通过 EP1 IN Interrupt
  - [x] SubTask 4.2: 实现 `usb_hid_send_text(text)` 复用 ASCII→HID 映射

- [x] Task 5: 整合主入口 `main.c`（修改 hidkbd_main.c）
  - [x] SubTask 5.1: 系统时钟初始化（60MHz）、UART1 初始化（CLI 物理串口）
  - [x] SubTask 5.2: BLE 初始化（HidEmu_Init + TMOS 任务启动）
  - [x] SubTask 5.3: USB 复合设备初始化（usb_composite_init + 中断使能）
  - [x] SubTask 5.4: 主循环：TMOS_SystemProcess + usb_composite_task

- [x] Task 6: 修改 CLI `type` 命令同时输出到 BLE 和 USB HID
  - [x] SubTask 6.1: 修改 `cmd_type` 调用 `hidkbd_type_text()`（BLE）和 `usb_hid_send_text()`（USB）
  - [x] SubTask 6.2: 添加 CDC 串口作为 CLI 输入备选（CDC RX → cli_rx_byte）

- [x] Task 7: 构建系统配置
  - [x] SubTask 7.1: 编写 `makefile.defs`（-include APP/subdir_extra.mk）
  - [x] SubTask 7.2: 编写 `APP/subdir_extra.mk`（filter-out 防重复，追加 usb_composite.c）
  - [x] SubTask 7.3: 生成 `obj/` 目录结构和 makefile（参照 HID_Keyboard obj/makefile 模板，修改源文件列表和链接脚本路径）

- [x] Task 8: 编译验证
  - [x] SubTask 8.1: 沙箱 `make all` 编译通过，无 error
  - [x] SubTask 8.2: 检查 ELF 大小在 Flash/RAM 限制内（FLASH 36.08%, RAM 100%）

# Task Dependencies
- [Task 2] depends on [Task 1]
- [Task 3] depends on [Task 2]
- [Task 4] depends on [Task 2]
- [Task 5] depends on [Task 3, Task 4]
- [Task 6] depends on [Task 4, Task 5]
- [Task 7] depends on [Task 1]
- [Task 8] depends on [Task 7, Task 5, Task 6]
