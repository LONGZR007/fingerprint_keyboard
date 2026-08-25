# 复合键盘工程（BLE HID + USB HID + CDC 串口）Spec

## Why
当前 BLE HID Keyboard demo 只有蓝牙键盘功能，调试依赖物理 UART。需要在单一工程中同时实现 BLE HID 键盘 + USB HID 键盘 + USB CDC 虚拟串口，使一个 CH583 芯片能同时通过蓝牙和 USB 向主机输入字符，并通过 USB CDC 提供免驱串口通道（可承载 CLI 或数据透传）。

## What Changes
- 在 `/workspace/combo_keyboard/` 新建独立工程，不修改 SDK 原始目录。
- 复制 `/workspace/sdk/CH583EVT/EVT/EXAM/SRC` 下的 `Ld/`、`RVMSIS/`、`Startup/`、`StdPeriphDriver/` 到新工程。
- 复制 BLE HID_Keyboard 的 `APP/`（含 CLI 框架）、`Profile/`、`HAL/`、`LIB/` 到新工程。
- 新增 `usb_composite.c`：实现 USB 复合设备（HID 键盘 + CDC ACM 串口），单路 USB 外设。
  - 设备描述符 class=0xEF/0x02/0x01（IAD），3 个接口：HID 键盘 + CDC 通信 + CDC 数据。
  - 端点分配：EP1=HID IN Interrupt, EP2=CDC 通知 IN Interrupt, EP3=CDC 数据 IN Bulk, EP4=CDC 数据 OUT Bulk。
  - CDC 类请求处理：SET_LINE_CODING / GET_LINE_CODING / SET_CONTROL_LINE_STATE。
  - CDC 数据收发 API：`usb_cdc_send()` / `usb_cdc_recv()`。
- 新增 `main.c`：初始化 BLE（TMOS 事件驱动）+ USB（中断 + 主循环延迟处理），主循环同时跑 TMOS 和 USB 处理。
- CLI 框架复用：`type` 命令同时向 BLE 和 USB HID 键盘发送字符；CDC 串口可作为 CLI 备选输入源。
- 构建系统：使用 `makefile.defs` + `subdir_extra.mk` 模式，不直接修改自动生成的 makefile。

## Impact
- Affected specs: 无（全新工程）
- Affected code: 新工程独立，不影响 SDK 原始 demo
- 硬件资源：EP0~EP4 全部占用；UART1（PA9/PA8）保留给物理串口 CLI；USB 外设使用第 1 路（`R8_USB_*` 寄存器）

## ADDED Requirements

### Requirement: USB 复合设备枚举
系统 SHALL 在单路 USB 上实现 IAD 复合设备，主机枚举后识别为 1 个 HID 键盘 + 1 个 CDC ACM 串口。

#### Scenario: 主机连接后枚举成功
- **WHEN** USB 连接到主机
- **THEN** 主机识别出 HID 键盘设备（接口 0）和 CDC 串口设备（接口 1+2），均免驱（Windows/Linux/macOS 自带驱动）

#### Scenario: HID 键盘报告发送
- **WHEN** 调用 USB HID 键盘发送函数
- **THEN** 通过 EP1 IN Interrupt 发送 8 字节 HID 键盘报告，主机收到键盘输入

### Requirement: CDC 串口数据收发
系统 SHALL 提供 CDC ACM 串口数据收发能力。

#### Scenario: 主机通过 CDC 发送数据
- **WHEN** 主机通过 CDC 串口发送数据到 EP4 OUT
- **THEN** 数据被接收并可通过回调或缓冲区读取

#### Scenario: 设备通过 CDC 上传数据
- **WHEN** 调用 `usb_cdc_send(buf, len)`
- **THEN** 数据通过 EP3 IN Bulk 发送到主机，主机串口终端收到

#### Scenario: 主机设置串口参数
- **WHEN** 主机发送 SET_LINE_CODING 请求
- **THEN** 设备保存波特率/数据位/停止位/校验参数，返回 ACK

### Requirement: BLE HID 键盘保持原有功能
系统 SHALL 保持 BLE HID 键盘的完整功能（广播、连接、配对、绑定、type 命令）。

#### Scenario: BLE 键盘 type 命令
- **WHEN** 串口输入 `type aB3`
- **THEN** BLE 键盘输出 `aB3`，同时 USB HID 键盘也输出 `aB3`

### Requirement: BLE 与 USB 共存
系统 SHALL 在主循环中同时运行 BLE TMOS 事件处理和 USB 中断延迟处理，两者不互相阻塞。

#### Scenario: BLE 连接期间 USB 可用
- **WHEN** BLE 已连接主机
- **THEN** USB CDC 串口仍可收发数据，USB HID 键盘仍可发送报告
