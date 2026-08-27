# Data Flash 用户数据存储接口实现计划

## 概要

参考 SDK 的 flash demo（`sdk/CH583EVT/EVT/EXAM/FLASH/src/Main.c`）与 EEPROM API（`ISP583.h` 中的 `EEPROM_READ/WRITE/ERASE` 宏），在 `combo_keyboard` 工程中实现一个基于 Data-Flash 的用户数据存储接口。

需求：
- 支持设置(set)、读取(get)、清除指定(delete)、清除全部(clear_all)
- 最多 50 个用户，每用户固定 128 字节槽位：用户名 16B + 用户数据 112B
- ID 映射固定区域：id0 → 偏移 0，id1 → 偏移 128，id_n → 偏移 n*128
- 空数据判定：用户名首字节 == 0xFF 且 用户数据首字节 == 0xFF 即视为无数据
- 清除时槽位须恢复为 0xFF（依赖 flash 擦除，因 flash 写只能 1→0，无法把 0 写成 0xFF）
- 通过 CLI `user` 子命令交互

## 当前状态分析（基于 Phase 1 探索）

### EEPROM API 关键事实
- `EEPROM_READ/WRITE/ERASE` 宏（`ISP583.h`）的 `StartAddr` 是 **Data-Flash 内的偏移地址**（demo: `EEPROM_READ(0, TestBuf, 500)`）。Data-Flash 共 32KB（`EEPROM_MAX_SIZE = 0x8000`），偏移范围 0 ~ 0x7FFF。
- Buffer 必须 4 字节对齐、位于 RAM。
- `EEPROM_MIN_ER_SIZE = EEPROM_PAGE_SIZE = 256`：**最小擦除单位 256 字节**。可一次擦除 256 的整数倍（demo 擦过 4096）。
- 写粒度 `EEPROM_MIN_WR_SIZE = 1`，可按字节写，但 256 对齐最优。
- flash 物理特性：擦除态 = 全 1 = 0xFF；写操作只能把 1 变 0，不能把 0 变 1。因此「把某槽位变回 0xFF」必须依赖擦除。

### 与 BLE_SNV 的地址冲突分析（关键）
- `HAL/include/CONFIG.h`: `BLE_SNV_ADDR = 0x77E00 - FLASH_ROM_MAX_SIZE = 0x77E00 - 0x070000 = 0x7E00`（即 Data-Flash 偏移 32256），`BLE_SNV_BLOCK=256, BLE_SNV_NUM=1`，占末尾约 512 字节（0x7E00 ~ 0x7FFF）。
- 用户数据区：偏移 0 ~ 6399（50 × 128 = 6400 = 0x1900）。
- 6400 < 32256 → **与 SNV 无重叠**，安全。

### 数据布局（固定槽位）
| 项 | 值 |
|---|---|
| `USER_FLASH_BASE` | 0x0000（Data-Flash 偏移） |
| `USER_SLOT_SIZE` | 128（16 + 112） |
| `USER_FLASH_MAX_USERS` | 50 |
| `USER_NAME_SIZE` | 16 |
| `USER_DATA_SIZE` | 112 |
| `USER_EMPTY_BYTE` | 0xFF |
| id → 偏移 | `id * 128`，id0=0, id1=128, …, id49=6272 |
| 总占用 | 0 ~ 6399（6400 字节） |
| 擦除页 | 256 字节/页 = 2 个槽位/页，共 25 页（id0&1→页0, id2&3→页1, …, id48&49→页24） |
| 与 SNV 间距 | 用户区结束 6400，SNV 起点 32256，间隔约 25.8KB |

### 项目约定（来自现有代码）
- CLI 命令模式：`cli_cmd_t` 表 + `cli_register_cmds()` + 子命令分发，参考 [fp_app_cmds.c](file:///workspace/combo_keyboard/APP/fp_app_cmds.c)。
- 模块初始化在 [hidkbd_main.c](file:///workspace/combo_keyboard/APP/hidkbd_main.c) `main()` 中调用，参考 `Fp_Init()`。
- 源文件经 [subdir_extra.mk](file:///workspace/combo_keyboard/APP/subdir_extra.mk) 追加到 `C_SRCS/C_DEPS/OBJS`（README 禁止直接改 Makefile）。
- 头文件统一放 `APP/include/`，源文件放 `APP/`。
- 包含 `CH58x_common.h`（经 `cli.h` 间接引入）即可使用 `EEPROM_*` 宏。

## 提议变更

### 1. 新建 `APP/include/user_flash.h`（头文件）
声明数据结构与 API。
- 宏：`USER_FLASH_MAX_USERS 50`、`USER_NAME_SIZE 16`、`USER_DATA_SIZE 112`、`USER_SLOT_SIZE 128`、`USER_FLASH_BASE 0x0000`、`USER_EMPTY_BYTE 0xFF`、`USER_FLASH_PAGE_SIZE 256`。
- 结构体 `user_record_t { char name[16]; uint8_t data[112]; }`（仅作文档/类型用，`sizeof == 128`，编译期 `static_assert` 校验）。
- API（返回 `BOOL`，FALSE=失败/空）：
  - `void  user_flash_init(void);`
  - `BOOL  user_flash_set(uint8_t id, const char *name, const uint8_t *data);` — name 以 `\0` 结尾字符串（≤15 字符），data 以 `\0` 结尾字符串（≤111 字符）；内部零填充到 16/112。
  - `BOOL  user_flash_get(uint8_t id, char *name, uint8_t *data);` — 空槽返回 FALSE 不拷贝；非空拷贝并保证 name/data 以 `\0` 结尾。
  - `BOOL  user_flash_delete(uint8_t id);`
  - `BOOL  user_flash_clear_all(void);`
  - `BOOL  user_flash_is_empty(uint8_t id);`
  - `uint8_t user_flash_count(void);` — 返回非空用户数（0~50）。

### 2. 新建 `APP/user_flash.c`（核心实现）
- 私有 4 字节对齐静态缓冲：`static __attribute__((aligned(4))) uint8_t s_page[256];`（页 RMW 用）、`static __attribute__((aligned(4))) uint8_t s_rec[128];`（单记录读用）。CLI 单线程 TMOS 调用，静态缓冲无重入风险。
- 私有 `page_rmw(id, new_rec_or_NULL)`：
  1. `page_id = id/2; slot = id%2; page_off = page_id*256;`
  2. `EEPROM_READ(page_off, s_page, 256)` 读整页；
  3. 目标 128B 槽：`new_rec` 非 NULL → `memcpy`；NULL（删除）→ `memset(..,0xFF,128)`；
  4. `EEPROM_ERASE(page_off, 256)` 擦整页；
  5. `EEPROM_WRITE(page_off, s_page, 256)` 写回整页（邻居槽原样恢复）。
  - 任一步返回非 0 即返回 FALSE。
- `user_flash_set`：参数校验 → 用 `memset(s_rec,0,128)` + `strncpy(name,16)` + `strncpy(data,112)` 装配记录（零填充，避免越界读源串）→ `page_rmw(id, s_rec)`。
- `user_flash_get`：`EEPROM_READ(id*128, s_rec, 128)` → 空判定 `s_rec[0]==0xFF && s_rec[16]==0xFF` → 空：返回 FALSE；非空：`memcpy` 出 name/data，并强制 `name[15]=0`、`data[111]=0`。
- `user_flash_delete`：`page_rmw(id, NULL)`（把目标槽置 0xFF）。
- `user_flash_clear_all`：循环擦除 25 页（`for p in 0..24: EEPROM_ERASE(p*256, 256)`），逐页判断返回值；全部成功才返回 TRUE。仅擦用户区 0~6399，**不触碰 SNV**。
- `user_flash_is_empty`：读 128B → `rec[0]==0xFF && rec[16]==0xFF`（读失败按空处理）。
- `user_flash_count`：遍历 50 个 id 调 `is_empty` 计数。
- `user_flash_init`：空操作（或仅打印容量信息）。**不在 init 中擦除**以免每次上电清空数据；出厂态默认 0xFF，空判定天然兼容；若需格式化由 CLI `user clear` 完成。
- 阻塞性：`EEPROM_*` 为同步阻塞调用，几 ms 量级，在 CLI TMOS 任务上下文调用可接受（与 BLE_SNV 自身写 flash 同一性质）。

### 3. 新建 `APP/user_flash_cli.c`（CLI 命令层）
仿 `fp_app_cmds.c`：`cmd_user` 子命令分发 + `s_user_cmds[]` 表 + `user_flash_cli_init()` 注册。
子命令：
- `user set <id> <name> <data...>` — name 单 token（≤15 字符），data 多 token 用空格拼接（≤111 字符），调 `user_flash_set`。
- `user get <id>` — 调 `user_flash_get`，打印 `id=N name="..." data="..."` 或 `empty`。
- `user del <id>` — 调 `user_flash_delete`。
- `user clear` — 调 `user_flash_clear_all`（可加二次确认提示）。
- `user list` — 遍历打印所有非空用户 id/name，末尾打印 `count=N`。
- `user count` — 打印非空用户数。
- `user help`（或无参数）— 打印子命令列表。
参数解析复用 `strtol`/`atoi`；id 范围校验 0~49。

### 4. 修改 `APP/hidkbd_main.c`
- 包含 `user_flash.h`。
- 在 `main()` 的 `Fp_Init();` 之后、`usb_composite_init();` 之前追加：
  ```c
  user_flash_init();        /* 用户数据存储模块初始化 */
  user_flash_cli_init();   /* 注册 user CLI 命令 */
  ```
- 增加 `extern void user_flash_cli_init(void);` 前向声明（或直接由头文件提供）。

### 5. 修改 `APP/subdir_extra.mk`
在 `_cli_srcs / _cli_deps / _cli_objs` 三行末尾追加 `user_flash.c` / `user_flash.c.d` / `user_flash.c.o` 三个对应项，沿用现有 `filter-out` 去重机制。
- `_cli_srcs` 追加 `../APP/user_flash.c ../APP/user_flash_cli.c`
- `_cli_deps` 追加 `./APP/user_flash.d ./APP/user_flash_cli.d`
- `_cli_objs` 追加 `./APP/user_flash.o ./APP/user_flash_cli.o`

## 假设与决策

1. **地址方案**：遵循用户明确指定「id0→0, id1→128」与 SDK demo `EEPROM_READ(0,…)`，采用 Data-Flash **偏移地址**（0 基），不与 SNV（偏移 0x7E00）冲突。
2. **删除/覆盖需页级 RMW**：因擦除粒度 256B > 槽位 128B，且写无法 0→1，故 set/delete 均走「读整页→改目标槽→擦页→写回整页」。邻居槽原样恢复。代价是写入量略增，但 50 用户键盘场景磨损可忽略。
3. **空判定**：`name[0]==0xFF && data[0]==0xFF`（data 首字节在偏移 16）。零填充的有效记录首字节为可见字符（≠0xFF），故不会误判为空。
4. **init 不擦除**：避免上电清空用户数据；依赖出厂 0xFF 态与空判定；格式化交给 `user clear`。
5. **同步阻塞调用**：在 CLI TMOS 任务上下文执行，几 ms 可接受；不引入额外 TMOS 周期任务（无异步需求）。
6. **缓冲对齐**：所有传给 `EEPROM_*` 的缓冲用 `__attribute__((aligned(4)))` 静态数组，满足 4 字节对齐要求。
7. **文件拆分**：核心 `user_flash.c` + CLI `user_flash_cli.c`，沿用项目 `fp_sm.c`/`fp_app_cmds.c` 的逻辑/CLI 分离约定。

## 验证步骤

1. **编译**：在沙箱用 `riscv64-unknown-elf-gcc` 工具链 + picolibc wrapper 构建 `combo_keyboard`，确认新增 2 个源文件编译链接通过、无未定义符号。
2. **CLI 自测（上板后）**：
   - `user help` → 列出子命令。
   - `user set 0 alice hello123` → `ok`；`user get 0` → 回显 `name="alice" data="hello123"`。
   - `user set 1 bob "secret data"` → `ok`；`user list` → 显示 id0/id1，`count=2`。
   - `user del 0` → `ok`；`user get 0` → `empty`；`user count` → `1`；`user get 1` → 仍正常（验证页 RMW 未误伤邻居）。
   - `user clear` → `ok`；`user list` → 空，`count=0`。
   - 重启后 `user list` 数据仍在（掉电保持）。
3. **SNV 完整性**：操作 `user` 命令后，BLE 仍能正常绑定/回连（验证未破坏 0x7E00 区）。
4. **边界**：`user set 50 …` → 拒绝（id≥50）；`user get 49` → 正常；`user set 0 <15字符名> <111字符数据>` → 正常存储与回读。
