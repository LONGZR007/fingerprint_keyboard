示例代码路径：sdk/CH583EVT/EVT/EXAM/BLE/HID_Keyboard/obj

# 沙箱编译环境适配方案

为让 CH583 BLE HID_Keyboard 例程在本沙箱（非 WCH MounRiver Studio 环境）通过编译，在未改动工程源码与自动生成 makefile 的前提下，做了以下工具链层适配：

1. **安装替代工具链**：安装 `gcc-riscv64-unknown-elf` + `picolibc-riscv64-unknown-elf`，并建立 `riscv-wch-elf-*` → `riscv64-unknown-elf-*` 软链，替代缺失的 WCH MounRiver 工具链。
2. **路径映射**：建立符号链接 `/home/long/github/fingerprint_keyboard` → `/workspace`，使 makefile 中写死的绝对路径生效。
3. **specs 补全**：编写 `nano.specs` / `nosys.specs`（对接 picolibc，替代 makefile 假定存在的新库 specs，补充 `-lc` / `-ldummyhost`）。
4. **gcc 包装器**：为 `riscv-wch-elf-gcc` 包一层 wrapper，自动将 `-march=rv32imac` 改写为 `rv32imac_zicsr_zifencei`（新版 GCC 需要 zicsr / zifencei 才能汇编 `csrr` / `fence.i`），并补 picolibc 的 `-isystem` / `-L` 路径。

编译结果：0 error，23 warning（均为厂商代码无害告警），生成 `HID_Keyboard.elf` / `HID_Keyboard.hex`。

# 项目管理

项目管理使用 `.wvproj` 文件，不能直接修改自动生成的 `makefile` / `subdir.mk` / `sources.mk` / `objects.mk`。MRS IDE 编译时会自动重新生成这些文件，手工修改会被覆盖。

## 添加自定义源文件

不修改任何自动生成的 makefile，通过 makefile 已有的 `-include ../makefile.defs` 钩子扩展：

1. **makefile.defs**（工程根目录）：MRS 生成的 `obj/makefile` 第 38 行已有 `-include ../makefile.defs`，此文件是用户扩展入口。
2. **APP/subdir_extra.mk**：通过 `makefile.defs` 间接 include，负责把新源文件追加到 `C_SRCS` / `C_DEPS` / `OBJS`。
3. **filter-out 防重复**：MRS IDE 重新生成 `subdir.mk` 时可能已把新文件加入列表，`subdir_extra.mk` 使用 `$(filter-out ...)` 只追加缺失的文件，避免 multiple definition 链接错误。

### 示例：subdir_extra.mk

```makefile
_cli_srcs := ../APP/cli.c ../APP/cli_uart.c ../APP/cli_app_cmds.c ../APP/cli_os_compat.c
_cli_deps := ./APP/cli.d ./APP/cli_uart.d ./APP/cli_app_cmds.d ./APP/cli_os_compat.d
_cli_objs := ./APP/cli.o ./APP/cli_uart.o ./APP/cli_app_cmds.o ./APP/cli_os_compat.o

C_SRCS += $(filter-out $(C_SRCS),$(_cli_srcs))
C_DEPS += $(filter-out $(C_DEPS),$(_cli_deps))
OBJS   += $(filter-out $(OBJS),$(_cli_objs))
```

> `APP/%.o: ../APP/%.c` pattern rule 已在 `subdir.mk` 中定义，无需额外编写编译规则。

# 串口命令行 (CLI) 分层设计

基于超时中断实现的轻量级串口命令行，支持参数解析，分层设计，方便更换串口。

## 文件结构

| 层级 | 文件 | 职责 |
|------|------|------|
| CLI 核心层 | `APP/include/cli.h` `APP/cli.c` | 行缓冲、回显、argc/argv 参数解析、命令分发 |
| UART HAL 层 | `APP/include/cli_uart.h` `APP/cli_uart.c` | 超时中断接收、RingBuf 缓冲、数据喂入 CLI |
| 应用命令层 | `APP/cli_app_cmds.c` | 示例命令：help/echo/info/reset/adv/led/baud |
| OS 兼容层 | `APP/cli_os_compat.c` | picolibc FILE 对象及系统调用存根 |
| 主工程接入 | `APP/hidkbd_main.c` | 初始化 CLI + 注册 TMOS 周期任务 |
| 源文件注册 | `APP/subdir_extra.mk` `makefile.defs` | 通过 makefile.defs 钩子追加新源文件（不修改自动生成的 makefile） |

## CLI 核心层

与硬件无关，负责命令行交互逻辑：

- **行缓冲**：逐字节接收，支持 BS/DEL 退格、CR/LF 归一
- **回显**：收到可打印字符即回显
- **参数解析**：`split_args()` 按空格/tab 分割为 `argv[0..argc-1]`
- **命令分发**：按命令名查找命令表并调用 handler，handler 签名为 `int (*handler)(int argc, char *argv[])`
- **API**：`cli_init` / `cli_register_cmd` / `cli_register_cmds` / `cli_rx_byte` / `cli_task` / `cli_print` / `cli_print_prompt`

## UART HAL 层

超时中断 + RingBuf，串口可方便更换：

- **超时中断**：利用 `UART_II_RECV_TOUT` 和 `UART_II_RECV_RDY` 中断，在一帧数据结束时自动触发，无需手动判断帧结束符
- **RingBuf**：256B 环形缓冲区，中断中写入，主循环中读取，避免数据丢失
- **串口切换**：修改 `cli_uart.h` 中的 `#define CLI_UART_PORT`（0/1/2/3），重新编译即可切换 UART0~UART3，CLI 核心层和应用命令层无需改动
- **两级宏展开**：通过 `_U_CAT3` 等宏将 `CLI_UART_PORT` 整数拼接到寄存器宏名中（如 `R8_UART1_RFC`）
- **默认配置**：UART1 (PA9 TX / PA8 RX)，115200 8N1

## 应用命令层

示例命令列表：

| 命令 | 参数 | 说明 |
|------|------|------|
| `help` | - | 列出所有命令 |
| `echo` | `<text...>` | 回显参数 |
| `info` | - | 显示设备信息 |
| `reset` | - | 软复位 |
| `adv` | `on\|off` | BLE 广播控制 |
| `led` | `<0\|1>` | LED 控制 |
| `baud` | `<rate>` | 修改 CLI 串口波特率 |

## OS 兼容层

解决 picolibc 链接问题：

- `stdin` / `stdout` / `stderr` 按 picolibc `FILE *const` ABI 定义为指针，挂 `struct __file.put/get/flush` vtable
- `put` 回调路由到 SDK 已有的 `_write()` 系统调用
- 不重复定义 `_sbrk`（复用 SDK `CH58x_sys.c` 的实现，避免 multiple definition）
- 提供其他系统调用存根：`_close` / `_fstat` / `_isatty` / `_lseek` / `_read` / `_kill` / `_getpid` / `_exit`

## 主工程接入

在 `hidkbd_main.c` 中调用 `Cli_Init()`（内部完成 UART 初始化 + 注册示例命令 + 注册 TMOS 周期任务），TMOS task 回调中调用 `cli_uart_drain()` + `cli_task()`，确保 CLI 处理不阻塞 BLE 协议栈。

# 手机距离监测自动锁屏 (Proximity Lock)

键盘同时维持两条 BLE 连接：电脑（HID 主连接，功能不变）+ 手机（监测连接，
配 ProxLock Android App）。键盘每 500ms 读手机连接 RSSI，EMA 平滑后，
低于阈值持续 5s → 自动向电脑发送 Win+L 锁屏；手机回到近处自动重新武装。

## 双连接模型

| 连接 | 角色 | 说明 |
|------|------|------|
| 主连接 | 电脑 | HID 键盘报告只走这条；`hiddev.c` 中 `gapConnHandle` 槽位 |
| 监测连接 | 手机 | App 写 Proximity Service Control 特征 0x01 激活 |

- 手机先连占了主槽位时，写 Control 激活会自动交换两个槽位（保证电脑永远持有主槽）。
- 主连接建立后广播保持低占空比（1s 间隔）运行，手机随时可连。
- 配对白名单：`prox pair` 开放 60s 配对窗口（`GAPBOND_AUTO_SYNC_WL` 临时关闭）。
- `PERIPHERAL_MAX_CONNECTION` 已改为 2（`HAL/include/CONFIG.h`），BLE heap 扩到 7KB。

## 模块与文件

| 文件 | 职责 |
|------|------|
| `Profile/hiddev.c/h` | [改] 双连接槽位、按 opcode 分流连接/断开、RSSI 回调挂载、配对窗口 |
| `APP/proxservice.c/h` | [新] Proximity GATT 服务（Control 写激活 / Status notify RSSI+状态） |
| `APP/proximity.c/h` | [新] 距离判定状态机（EMA、双阈值迟滞、冷却、Win+L 触发）、DataFlash 配置 |
| `APP/prox_cli.c` | [新] `rssi` / `prox` CLI 命令 |
| `APP/hidkbd.c/h` | [改] `hidkbd_send_combo()` 组合键、状态回调按主连接过滤 |
| `APP/keyboard_dispatch.c/h` | [改] `keyboard_send_combo(mod,key,ch)` 双通道分发 |
| `android_prox_app/` | [新] ProxLock App（Kotlin+Compose，前台服务保活、自动重连） |

协议契约（App 与固件两端一致）：

```
Service  a5f5aa00-c263-4a0c-8e8f-9c0b7a5d3e01
Control  a5f5aa01-...   Write 0x01 激活 / 0x00 停止（要求加密链路）
Status   a5f5aa02-...   Notify [int8 rssi][state][flags]
                        state: 0=armed 1=triggered 2=cooldown 3=off
```

配置存 DataFlash `0x7000`（独立页，用户区 0~6399 与 SNV 之外）。

## 编译

```
export PATH="$HOME/github/wch/WCHISPTool_CMD/Linux/bin/x64:$PATH"
export PATH="$HOME/github/wch/MRS_Toolchain_Linux_X64_V240/Toolchain/RISC-V Embedded GCC12/bin:$PATH"
cd combo_keyboard/obj/
make clean && make -j8 all
```

产物：`combo_keyboard.hex`（WCHISPTool 烧录）。

## 联调与校准清单

1. **刷固件**：烧录 `combo_keyboard.hex`，电脑重连键盘，确认打字/指纹/CLI 正常（回归基线）。
2. **配对手机**：串口执行 `prox pair`（60s 窗口）→ 手机系统蓝牙配对 `HID Keyboard`。
3. **连接验证**：手机打开 ProxLock → 开始守护 → 显示「守护中 · -xx dBm」；
   键盘串口 `prox show` 应显示 `monitor: active`，且电脑键盘输入不受影响。
4. **触发链路**：串口 `prox test` → 电脑应立即锁屏（验证 Win+L + 通道）。
5. **现场校准**：手机放身上正常姿势，`rssi` 记录典型读数（一般 -60~-70），
   `prox thr <典型值-8>`；`prox exit` 保持比 `thr` 高 8dB；确认 `prox confirm 5`。
6. **真机验证**：手机装兜里离开电脑 10m+，约 5~8s 后电脑锁屏；回到工位 App 自动重连，
   冷却 60s 后回到 armed（App 状态卡显示）。
7. **回归检查**：BLE 打字、USB 打字、`type` 命令、指纹录入/验证、
   `bond erase`（会断开所有连接并擦绑表）、广播回连。

## 注意事项

- 电脑断连后设备恢复广播（bonded→低占空比长广播），电脑可随时回连。
- 手机 App 被系统杀死时默认**不会**触发锁屏（`prox lost 0`）；若需要
  「失联即锁」语义可 `prox lost 30`（失联 30s 视为离开）。
- Win+L 通道由 `prox ch` 配置：`auto`（BLE 优先）/`ble`/`usb`/`both`，持久化保存。
