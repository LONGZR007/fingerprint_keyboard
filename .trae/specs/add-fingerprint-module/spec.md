# 串口指纹模组驱动 Spec

## Why

combo\_keyboard 工程已实现 BLE HID + USB HID + CDC 串口 + CLI 交互，但缺少指纹识别能力。需要在 CH583 上通过 UART3 (PA5 TX / PA4 RX) 接入指纹模组，实现指纹注册、验证、删除、清空等功能，并集成到现有 CLI 和主循环中。

## What Changes

* 新增 **UART HAL 层** `fp_uart.h` / `fp_uart.c`：复用 cli\_uart 的分层模式（超时中断 + RingBuf + 可换串口），默认 UART3 (PA5/PA4)，57600 8N1。

* 新增 **协议解析层** `fp_proto.h` / `fp_proto.c`：单字节接收状态机（switch），解析指纹模组数据包格式（`EF01` + 设备地址 4B + 包标识 1B + 包长度 2B + 数据 N + 校验和 2B），校验和验证，完整包回调。

* 新增 **极简 protothread 库** `APP/include/pt.h`：header-only 宏库（PT\_BEGIN / PT\_WAIT\_UNTIL / PT\_SPAWN 等），用于状态机内顺序编写「发指令→等应答」逻辑而不阻塞主循环。

* 新增 **指纹状态机层** `fp_sm.h` / `fp_sm.c`：定义状态（IDLE / VERIFY / ENROLL / DELETE\_ONE / CLEAR\_ALL），每个状态用 protothread 编写指令收发流程，所有结果通过回调通知应用层。

  * 注册使用 `PS_AutoEnroll(0x31)`，默认配置可调宏定义。

  * 验证使用 `PS_AutoIdentify(0x32)`，支持 1:N 全库搜索。

  * 删除指定使用 `PS_DeletChar(0x0C)`。

  * 清空使用 `PS_Empty(0x0D)`。

  * 取消使用 `PS_Cancel(0x30)`。

* 新增 **CLI 指纹命令层** `fp_app_cmds.c`：注册 `fp` 子命令（enroll / verify / delete / clear / status / cancel），控制状态机切换。

* 修改 `hidkbd_main.c`：初始化 fp\_uart + fp\_proto + fp\_sm，注册 TMOS 周期任务驱动指纹模组解析和状态机。

* 修改 `APP/subdir_extra.mk`：追加新增源文件。

## Impact

* Affected specs: `create-combo-keyboard-project`（在其工程基础上扩展）

* Affected code:

  * 新增: `APP/include/fp_uart.h`, `APP/fp_uart.c`, `APP/include/fp_proto.h`, `APP/fp_proto.c`, `APP/include/pt.h`, `APP/include/fp_sm.h`, `APP/fp_sm.c`, `APP/fp_app_cmds.c`

  * 修改: `APP/hidkbd_main.c`（初始化 + 任务注册）, `APP/subdir_extra.mk`（源文件追加）

* 硬件资源: UART3 (PA5 TX / PA4 RX)，与 CLI 使用的 UART1 不冲突

## ADDED Requirements

### Requirement: 指纹 UART HAL 层

系统 SHALL 提供独立的指纹串口 HAL，复用 CLI 的超时中断 + RingBuf 接收模式，默认 UART3 (PA5 TX / PA4 RX)、57600 bps 8N1（匹配指纹模组默认波特率）。

#### Scenario: 指纹模组数据接收

* **WHEN** 指纹模组通过 UART3 发送应答数据

* **THEN** 超时中断触发，数据写入 RingBuf，`fp_uart_drain()` 在任务上下文逐字节喂入协议解析层

#### Scenario: 指纹模组指令发送

* **WHEN** 调用 `fp_uart_send(buf, len)`

* **THEN** 数据通过 UART3 阻塞发送到指纹模组

#### Scenario: 串口可更换

* **WHEN** 修改 `fp_uart.h` 中的 `FP_UART_PORT` 宏

* **THEN** 指纹串口切换到对应 UART，无需修改其他层代码

### Requirement: 指纹协议解析层（单字节 switch 状态机）

系统 SHALL 通过单字节 switch 状态机解析指纹模组数据包，逐字节接收、自动组装完整包、校验和验证、回调通知。

#### Scenario: 完整应答包解析

* **WHEN** 逐字节收到 `EF01` + 设备地址(4B) + 包标识(1B) + 包长度(2B) + 数据(N) + 校验和(2B)

* **THEN** 校验和通过后，调用注册的回调函数，传递包标识、确认码、数据指针和长度

#### Scenario: 校验和错误丢弃

* **WHEN** 接收到的包校验和不匹配

* **THEN** 丢弃该包，状态机复位到等待包头 `EF01`，不通知上层

#### Scenario: 部分包超时复位

* **WHEN** 接收过程中长时间（如 500ms）无新字节

* **THEN** 状态机复位到等待包头状态，避免残留状态卡死

### Requirement: 极简 Protothread 库

系统 SHALL 提供 header-only 的 protothread 宏库（`pt.h`），支持在状态机内用顺序代码编写「发指令→等应答→处理」流程而不阻塞主循环。

#### Scenario: 非阻塞等待应答

* **WHEN** 状态机发送指令后等待应答

* **THEN** protothread 让出执行权（返回 PT\_FALSE），主循环继续运行 BLE/USB/CLI；下次调用时从等待点恢复，应答到达后继续执行后续逻辑

### Requirement: 指纹状态机

系统 SHALL 实现指纹状态机，支持以下状态：IDLE（空闲）、ENROLL（自动注册）、VERIFY（自动验证）、DELETE\_ONE（删除指定）、CLEAR\_ALL（清空全部）。上电默认进入 VERIFY 状态可配。

#### Scenario: 自动注册指纹

* **WHEN** CLI 发出 `fp enroll <id>` 命令

* **THEN** 状态机切换到 ENROLL，发送 `PS_AutoEnroll(0x31)` 指令，按注册配置参数执行；过程中收到步骤应答（采图成功/生成特征/手指离开/合并/存储）时通过回调通知应用层；成功或失败后回到 IDLE

#### Scenario: 自动验证指纹（上电默认）

* **WHEN** 状态机处于 VERIFY 或 CLI 发出 `fp verify` 命令

* **THEN** 发送 `PS_AutoIdentify(0x32)` 指令（1:N 全库搜索），等待采集→生成特征→搜索比对；成功时回调通知 PageID + 得分，失败时回调通知原因，之后回到 IDLE 或重新进入 VERIFY

#### Scenario: 删除指定指纹

* **WHEN** CLI 发出 `fp delete <id>` 命令

* **THEN** 状态机发送 `PS_DeletChar(0x0C)` 指令删除指定 ID 的模板，结果通过回调通知

#### Scenario: 清空指纹库

* **WHEN** CLI 发出 `fp clear` 命令

* **THEN** 状态机发送 `PS_Empty(0x0D)` 指令清空全部模板，结果通过回调通知

#### Scenario: 取消当前操作

* **WHEN** CLI 发出 `fp cancel` 命令

* **THEN** 状态机发送 `PS_Cancel(0x30)` 指令终止当前自动流程，回到 IDLE

#### Scenario: 忙状态拒绝

* **WHEN** 状态机正在执行某操作时收到新的操作请求

* **THEN** 拒绝新请求并返回忙状态提示

### Requirement: 注册默认配置可调

系统 SHALL 通过宏定义提供注册参数默认配置，集中管理、方便修改。

#### Scenario: 默认注册配置

* **WHEN** 使用默认配置注册指纹

* **THEN** Param 值按以下位组合（可在 `fp_sm.h` 中通过宏修改）：

  * `bit0 = 1`：采图成功后 LED 灭

  * `bit2 = 0`：要求返回关键步骤

  * `bit3 = 1`：允许覆盖已有 ID

  * `bit4 = 0`：允许重复注册

  * `bit5 = 0`：每次采集后要求手指离开

  * 默认录入次数、超时等待时间也可通过宏配置

### Requirement: 指纹结果消息通知

系统 SHALL 为所有指纹操作结果预留统一的消息回调机制，通知应用层。

#### Scenario: 注册成功通知

* **WHEN** 指纹注册成功

* **THEN** 回调 `FP_MSG_ENROLL_OK`，携带注册的 PageID

#### Scenario: 注册步骤通知

* **WHEN** 注册过程中收到步骤应答（如「请按手指」「请移开手指」）

* **THEN** 回调 `FP_MSG_ENROLL_STEP`，携带步骤码和当前录入次数

#### Scenario: 验证成功通知

* **WHEN** 指纹验证成功

* **THEN** 回调 `FP_MSG_VERIFY_OK`，携带匹配的 PageID 和得分

#### Scenario: 验证失败通知

* **WHEN** 指纹验证失败（未搜索到 / 残留指纹 / 超时）

* **THEN** 回调 `FP_MSG_VERIFY_FAIL`，携带失败原因码

### Requirement: CLI 指纹命令

系统 SHALL 通过 CLI 提供 `fp` 命令族控制指纹模组。

#### Scenario: fp 命令列表

* **WHEN** 输入 `fp` 或 `fp help`

* **THEN** 显示子命令列表：enroll / verify / delete / clear / status / cancel

#### Scenario: fp enroll

* **WHEN** 输入 `fp enroll 5`

* **THEN** 启动 ID=5 的指纹注册流程，CLI 输出进度提示

#### Scenario: fp verify

* **WHEN** 输入 `fp verify`

* **THEN** 启动 1:N 全库验证流程

#### Scenario: fp delete

* **WHEN** 输入 `fp delete 3`

* **THEN** 删除 ID=3 的指纹模板

#### Scenario: fp clear

* **WHEN** 输入 `fp clear`

* **THEN** 清空指纹库所有模板

#### Scenario: fp status

* **WHEN** 输入 `fp status`

* **THEN** 显示当前状态机状态（IDLE/ENROLL/VERIFY/...）

#### Scenario: fp cancel

* **WHEN** 输入 `fp cancel`

* **THEN** 终止当前操作回到 IDLE

### Requirement: 主循环集成

系统 SHALL 在主循环 / TMOS 任务中驱动指纹模组的协议解析和状态机推进，不阻塞 BLE/USB/CLI。

#### Scenario: 周期任务驱动

* **WHEN** TMOS 周期任务触发（如每 5ms）

* **THEN** 调用 `fp_uart_drain()` 喂入接收字节 → `fp_proto_task()` 解析包 → `fp_sm_task()` 推进状态机 protothread

