# Tasks

- [ ] Task 1: 创建极简 protothread 库 `APP/include/pt.h`
  - [ ] SubTask 1.1: 定义 `pt_ctx_t`（仅一个 `uint16_t lc` 行号变量）和核心宏：`PT_INIT(ctx)` / `PT_BEGIN(ctx)` / `PT_END(ctx)` / `PT_WAIT_UNTIL(ctx, cond)` / `PT_SPAWN(ctx, child, fn)` / `PT_EXIT(ctx)` / `PT_RESTART(ctx)`
  - [ ] SubTask 1.2: 编写注释说明使用方式和注意事项（不可有局部变量跨 PT_WAIT_UNTIL 保持、需在任务上下文周期调用）

- [ ] Task 2: 实现指纹 UART HAL 层 `fp_uart.h` / `fp_uart.c`
  - [ ] SubTask 2.1: 编写 `fp_uart.h`：宏 `FP_UART_PORT`（默认 3）、`FP_UART_BAUD`（默认 57600）、`FP_RING_SIZE`（256），API 声明 `fp_uart_init` / `fp_uart_drain` / `fp_uart_send` / `fp_uart_available`
  - [ ] SubTask 2.2: 编写 `fp_uart.c`：复用 cli_uart.c 的两级宏展开模式（U/U_R8/U_FN 等），RX 超时中断 + RingBuf，TX 阻塞发送，中断处理函数 `FP_UART_IRQHandler`（UART3_IRQHandler）

- [ ] Task 3: 实现指纹协议解析层 `fp_proto.h` / `fp_proto.c`
  - [ ] SubTask 3.1: 编写 `fp_proto.h`：定义包结构常量（`FP_HEAD0=0xEF`、`FP_HEAD1=0x01`、PID 值）、解析状态枚举、完整包回调函数指针类型 `fp_packet_cb_t(pid, data, len)`，API 声明 `fp_proto_init` / `fp_proto_rx_byte` / `fp_proto_task` / `fp_proto_set_callback` / `fp_proto_packet_ready`
  - [ ] SubTask 3.2: 编写 `fp_proto.c`：单字节 switch 状态机（WAIT_HEAD0 → WAIT_HEAD1 → ADDR(4) → PID → LEN_HI → LEN_LO → DATA → CHK_HI → CHK_LO），校验和计算与验证，完整包回调，超时复位逻辑（基于系统 tick）
  - [ ] SubTask 3.3: 提供组装指令包的辅助函数 `fp_proto_build_cmd(buf, cmd, params, param_len)` 和 `fp_proto_checksum`（供状态机层使用）

- [ ] Task 4: 实现指纹状态机层 `fp_sm.h` / `fp_sm.c`
  - [ ] SubTask 4.1: 编写 `fp_sm.h`：状态枚举（FP_IDLE / FP_ENROLL / FP_VERIFY / FP_DELETE_ONE / FP_CLEAR_ALL / FP_CANCEL）、消息枚举（FP_MSG_ENROLL_OK / FP_MSG_ENROLL_STEP / FP_MSG_VERIFY_OK / FP_MSG_VERIFY_FAIL / FP_MSG_DELETE_OK / FP_MSG_DELETE_FAIL / FP_MSG_CLEAR_OK / FP_MSG_CLEAR_FAIL / FP_MSG_TIMEOUT / FP_MSG_CANCELLED）、注册配置宏（`FP_ENROLL_TIMES` / `FP_ENROLL_PARAM` / `FP_VERIFY_SCORE_LEVEL` / `FP_WAIT_TIMEOUT_MS`）、回调类型 `fp_notify_cb_t`，API 声明 `fp_sm_init` / `fp_sm_task` / `fp_sm_enroll` / `fp_sm_verify` / `fp_sm_delete` / `fp_sm_clear` / `fp_sm_cancel` / `fp_sm_get_state` / `fp_sm_set_notify_cb`
  - [ ] SubTask 4.2: 编写 `fp_sm.c` 全局状态：当前状态 `s_state`、protothread 上下文 `s_pt`、接收应答缓冲 `s_rx_pkt`（PID + 数据 + 长度 + ready 标志 + tick 时间戳）、通知回调 `s_notify`
  - [ ] SubTask 4.3: 实现 `fp_proto` 回调 → 写入 `s_rx_pkt` 并置 ready 标志（供 protothread 等待消费）
  - [ ] SubTask 4.4: 实现 ENROLL protothread：`PT_BEGIN` → 组装 0x31 指令(ID, Times, Param) → `fp_uart_send` → `PT_WAIT_UNTIL(s_rx_pkt.ready || timeout)` → 解析确认码 → 循环处理步骤应答（00 00 00 → 00 01 n → 00 02 n → 00 03 n → ... → 00 04 F0 → 00 05 F1 → 00 06 F2）→ 每步回调通知 → 成功/失败回调 → `PT_END`。回到 IDLE。
  - [ ] SubTask 4.5: 实现 VERIFY protothread：`PT_BEGIN` → 组装 0x32 指令(ScoreLevel, ID=0xFFFF, Param) → send → wait → 解析确认码 → 处理步骤（00 00 → 00 01 → 00 05 PageID Score）→ 成功/失败回调 → `PT_END`。回到 IDLE 或重新进入 VERIFY（可配）。
  - [ ] SubTask 4.6: 实现 DELETE_ONE protothread：组 0x0C 指令(PageID, N=1) → send → wait → 确认码回调。
  - [ ] SubTask 4.7: 实现 CLEAR_ALL protothread：组 0x0D 指令 → send → wait → 确认码回调。
  - [ ] SubTask 4.8: 实现 CANCEL 逻辑：组 0x30 指令 → send → wait → 复位状态机。
  - [ ] SubTask 4.9: 实现 `fp_sm_task`：调用 protothread 当前状态对应函数，处理超时检测。
  - [ ] SubTask 4.10: 实现状态切换 API（`fp_sm_enroll` 等）：检查忙状态 → 设置目标 ID/参数 → 切换状态 → 初始化 protothread。

- [ ] Task 5: 实现 CLI 指纹命令层 `fp_app_cmds.c`
  - [ ] SubTask 5.1: 注册 `fp` 命令到 CLI 命令表
  - [ ] SubTask 5.2: 实现 `cmd_fp`：解析子命令（enroll/verify/delete/clear/status/cancel），调用对应 `fp_sm_*` API，输出结果提示
  - [ ] SubTask 5.3: 指纹通知回调函数：将消息格式化为 CLI 文本输出（如「指纹注册成功，ID=5」「验证通过，ID=3，得分=120」）

- [ ] Task 6: 集成到主工程 `hidkbd_main.c`
  - [ ] SubTask 6.1: 新增 `Fp_Init()` 函数：调用 `fp_uart_init` → `fp_proto_init` → `fp_sm_init`，注册 TMOS 周期任务
  - [ ] SubTask 6.2: 新增 TMOS 任务处理函数：调用 `fp_uart_drain` → `fp_proto_task` → `fp_sm_task`，5ms 周期
  - [ ] SubTask 6.3: 在 `main()` 中 `Cli_Init()` 后调用 `Fp_Init()`

- [ ] Task 7: 更新构建配置 `APP/subdir_extra.mk`
  - [ ] SubTask 7.1: 追加 `fp_uart.c` / `fp_proto.c` / `fp_sm.c` / `fp_app_cmds.c` 到 `_cli_srcs` / `_cli_deps` / `_cli_objs`

- [ ] Task 8: 编译验证
  - [ ] SubTask 8.1: 沙箱 `make all` 编译通过，无 error
  - [ ] SubTask 8.2: 检查新增文件无 multiple definition 链接错误

# Task Dependencies
- [Task 2] depends on [Task 1]（fp_uart 不依赖 pt，但整体需要先有 pt 库）
- [Task 3] depends on [Task 2]（协议层需要 UART HAL 的 drain 喂入字节）
- [Task 4] depends on [Task 1, Task 3]（状态机需要 pt 库 + 协议层解析）
- [Task 5] depends on [Task 4]（CLI 命令需要状态机 API）
- [Task 6] depends on [Task 2, Task 3, Task 4, Task 5]
- [Task 7] depends on [Task 2, Task 3, Task 4, Task 5]（所有新源文件就绪后追加）
- [Task 8] depends on [Task 6, Task 7]
