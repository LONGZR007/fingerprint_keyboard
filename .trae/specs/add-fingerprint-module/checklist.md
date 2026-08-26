# Checklist

## 架构分层
- [x] 指纹 UART HAL 层 `fp_uart.h` / `fp_uart.c` 与 CLI 的 `cli_uart` 分层模式一致（超时中断 + RingBuf + 可换串口宏）
- [x] 指纹 UART HAL 默认 UART3 (PA5 TX / PA4 RX)、57600 bps 8N1（匹配指纹模组默认波特率）
- [x] 协议解析层 `fp_proto.h` / `fp_proto.c` 使用单字节 switch 状态机解析包格式（EF01 + addr + PID + len + data + checksum）
- [x] protothread 库 `pt.h` 为 header-only 宏库，无 C 文件依赖（使用用户上传的 Adam Dunkels 版本）

## 协议解析
- [x] 状态机覆盖 WAIT_HEAD0 → WAIT_HEAD1 → ADDR(4) → PID → LEN_HI → LEN_LO → DATA → CHK_HI → CHK_LO 全流程
- [x] 校验和计算范围正确（从 PID 到校验和前最后一字节求和取低 16 位，不含设备地址）
- [x] 校验和错误时丢弃包并复位状态机
- [x] 部分包超时（100 次 tick ≈ 500ms 无新字节）自动复位到等待包头状态
- [x] 完整包通过回调通知上层（PID + 数据指针 + 数据长度）

## Protothread 状态机
- [x] ENROLL 使用 `PS_AutoEnroll(0x31)`，Param 按配置宏组合（bit0=1, bit2=0, bit3=1, bit4=0, bit5=0 = 0x09）
- [x] ENROLL protothread 正确处理多步骤应答（合法性检测 → 采图 → 生成特征 → 手指离开循环 → 合并 → 重复检测 → 存储）
- [x] VERIFY 使用 `PS_AutoIdentify(0x32)`，TargetID=0xFFFF（1:N 全库搜索）
- [x] DELETE_ONE 使用 `PS_DeletChar(0x0C)`，PageID + N=1
- [x] CLEAR_ALL 使用 `PS_Empty(0x0D)`
- [x] CANCEL 使用 `PS_Cancel(0x30)`
- [x] 所有 protothread 在等待应答时不阻塞主循环（PT_WAIT_UNTIL 返回 PT_WAITING 让出）
- [x] 所有 protothread 有超时保护（FP_WAIT_TIMEOUT_MS=5000，超时后通知 FP_MSG_TIMEOUT 并回到 IDLE）
- [x] 状态切换时检查忙状态，忙时拒绝新请求（FP_MSG_BUSY）

## 注册配置
- [x] `fp_sm.h` 中通过宏集中定义注册参数：录入次数、Param 位配置、超时时间
- [x] 默认 Param = bit0(1) | bit2(0) | bit3(1) | bit4(0) | bit5(0) = 0x09
- [x] 录入次数默认值 FP_ENROLL_TIMES=3（可配）
- [x] 等待手指超时时间 FP_WAIT_TIMEOUT_MS=5000（可配）

## 消息通知
- [x] 所有操作结果通过 `fp_notify_cb_t` 回调通知应用层
- [x] ENROLL_OK 携带 PageID
- [x] ENROLL_STEP 携带步骤码 + 当前录入次数
- [x] VERIFY_OK 携带 PageID + 得分
- [x] VERIFY_FAIL 携带失败原因码
- [x] DELETE / CLEAR 成功/失败均有对应消息
- [x] TIMEOUT / CANCELLED / BUSY 有对应消息

## CLI 命令
- [x] `fp enroll <id>` 启动注册
- [x] `fp verify` 启动验证
- [x] `fp delete <id>` 删除指定
- [x] `fp clear` 清空全部
- [x] `fp status` 显示状态机当前状态
- [x] `fp cancel` 取消当前操作
- [x] 指纹回调消息格式化为 CLI 文本输出
- [x] 忙状态时 CLI 输出拒绝提示

## 主循环集成
- [x] `hidkbd_main.c` 中 `Fp_Init()` 初始化 UART + 协议层 + 状态机 + 注册 CLI 命令
- [x] TMOS 周期任务（5ms）调用 `fp_uart_drain` → `fp_proto_task` → `fp_sm_task`
- [x] 不阻塞 BLE TMOS 事件调度和 USB composite_task

## 构建配置
- [x] `APP/subdir_extra.mk` 追加 `fp_uart.c` / `fp_proto.c` / `fp_sm.c` / `fp_app_cmds.c`
- [x] 使用 `filter-out` 防止重复定义
- [x] 沙箱 `make all` 编译通过无 error（FLASH 37.51%, RAM 100%）
