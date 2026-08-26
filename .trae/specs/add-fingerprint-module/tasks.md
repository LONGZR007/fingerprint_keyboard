# Tasks

- [x] Task 1: 极简 protothread 库（用户已上传 `APP/pt/pt.h` + `lc.h` + `lc-switch.h`）
  - [x] SubTask 1.1: 使用标准 Adam Dunkels protothread 宏库
  - [x] SubTask 1.2: 通过 `#include "../pt/pt.h"` 从 APP/include/ 正确引用

- [x] Task 2: 实现指纹 UART HAL 层 `fp_uart.h` / `fp_uart.c`
  - [x] SubTask 2.1: `fp_uart.h`：宏 `FP_UART_PORT=3`、`FP_UART_BAUD=57600`、`FP_RING_SIZE=256`，API 声明
  - [x] SubTask 2.2: `fp_uart.c`：复用 cli_uart.c 两级宏展开，RX 超时中断 + RingBuf，TX 阻塞，UART3_IRQHandler

- [x] Task 3: 实现指纹协议解析层 `fp_proto.h` / `fp_proto.c`
  - [x] SubTask 3.1: `fp_proto.h`：包结构常量、解析状态枚举、回调类型、API 声明
  - [x] SubTask 3.2: `fp_proto.c`：单字节 switch 状态机（9 状态），校验和计算（PID+len+data），超时复位
  - [x] SubTask 3.3: `fp_proto_build_cmd()` 和 `fp_proto_checksum()` 辅助函数

- [x] Task 4: 实现指纹状态机层 `fp_sm.h` / `fp_sm.c`
  - [x] SubTask 4.1: `fp_sm.h`：状态枚举、消息枚举、配置宏（`FP_ENROLL_TIMES=3`/`FP_ENROLL_PARAM=0x09`/`FP_VERIFY_SCORE_LEVEL=2`/`FP_WAIT_TIMEOUT_MS=5000`）、回调类型、API 声明
  - [x] SubTask 4.2: `fp_sm.c` 全局状态：`s_state`/`s_pt`/`s_rx`/`s_notify`/`s_tick`
  - [x] SubTask 4.3: `on_packet_cb` 回调 → memcpy 到 `s_rx` + ready 标志
  - [x] SubTask 4.4: ENROLL protothread：0x31 指令，多步骤循环处理（0x00~0x06），FP_WAIT_RESPONSE 宏
  - [x] SubTask 4.5: VERIFY protothread：0x32 指令，1:N 搜索，PageID+Score 解析
  - [x] SubTask 4.6: DELETE_ONE protothread：0x0C 指令（PageID, N=1）
  - [x] SubTask 4.7: CLEAR_ALL protothread：0x0D 指令
  - [x] SubTask 4.8: CANCEL protothread：0x30 指令
  - [x] SubTask 4.9: `fp_sm_task()`：s_tick++ + switch(s_state) 调度对应 protothread
  - [x] SubTask 4.10: 状态切换 API：忙状态检查 + PT_INIT + 状态切换

- [x] Task 5: 实现 CLI 指纹命令层 `fp_app_cmds.c`
  - [x] SubTask 5.1: `s_fp_cmds[]` 命令表 + `cli_register_cmds()` 注册
  - [x] SubTask 5.2: `cmd_fp` 子命令分发：enroll/verify/delete/clear/status/cancel/help
  - [x] SubTask 5.3: `fp_msg_handler` 通知回调：12 种消息格式化为 CLI 文本

- [x] Task 6: 集成到主工程 `hidkbd_main.c`
  - [x] SubTask 6.1: `Fp_Init()`：fp_uart_init → fp_proto_init → fp_sm_init → fp_app_cmds_init + TMOS 注册
  - [x] SubTask 6.2: `FpTask_ProcessEvent()`：fp_uart_drain → fp_proto_task → fp_sm_task，5ms 周期
  - [x] SubTask 6.3: `main()` 中 `Cli_Init()` 后调用 `Fp_Init()`

- [x] Task 7: 更新构建配置 `APP/subdir_extra.mk`
  - [x] SubTask 7.1: 追加 `fp_uart.c` / `fp_proto.c` / `fp_sm.c` / `fp_app_cmds.c` + .d + .o

- [x] Task 8: 编译验证
  - [x] SubTask 8.1: `make all` 编译通过，无 error（FLASH 37.51%, RAM 100%）
  - [x] SubTask 8.2: 无 multiple definition 链接错误

# Task Dependencies
- [Task 2] depends on [Task 1]
- [Task 3] depends on [Task 2]
- [Task 4] depends on [Task 1, Task 3]
- [Task 5] depends on [Task 4]
- [Task 6] depends on [Task 2, 3, 4, 5]
- [Task 7] depends on [Task 2, 3, 4, 5]
- [Task 8] depends on [Task 6, 7]
