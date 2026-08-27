# Data Flash 用户数据存储接口实现计划

## Summary

在 combo\_keyboard 工程中新增 Data-Flash (EEPROM) 用户数据存储模块，支持设置、读取、删除指定、清空全部，基于 ID 映射固定区域。50 个用户，每用户 128 字节（16B 用户名 + 112B 用户数据），使用 SDK 的 EEPROM\_READ/ERASE/WRITE 宏操作 Data-Flash。

## Current State Analysis

### SDK Flash API（ISP583.h）

* `EEPROM_READ(StartAddr, Buffer, Length)` — 读取 Data-Flash，最小 1 字节

* `EEPROM_ERASE(StartAddr, Length)` — 擦除 Data-Flash，最小 256 字节/页

* `EEPROM_WRITE(StartAddr, Buffer, Length)` — 写入 Data-Flash，最小 1 字节，最佳 256 字节倍数

* `EEPROM_PAGE_SIZE = 256`，`EEPROM_BLOCK_SIZE = 4096`，`EEPROM_MAX_SIZE = 0x8000`（32KB）

### Flash 空间布局

* Data-Flash 总大小：32KB（0x0000-0x7FFF）

* BLE SNV 绑定信息：占用 0x7E00-0x7EFF（256B），用户数据从 0x0000 开始不冲突

* 用户数据区域：0x0000-0x19FF（50 × 128 = 6400 字节），安全无冲突

### 擦除限制

* EEPROM 最小擦除单位 = 256 字节（一页 = 2 个用户）

* 清除单个用户（128B）需要：读取整页(256B) → 填充目标 128B 为 0xFF → 擦除整页 → 写回整页

### 项目现有模式

* 分层：`APP/include/*.h` + `APP/*.c`，通过 `subdir_extra.mk` 注册源文件

* CLI 命令：`cli_app_cmds.c` / `fp_app_cmds.c` 模式，用 `cli_register_cmds()` 注册子命令表

## Proposed Changes

### 1. 新增 `APP/include/user_flash.h`

数据结构和宏定义：

```c
#define USER_FLASH_MAX_USERS    50
#define USER_NAME_SIZE          16
#define USER_DATA_SIZE          112
#define USER_SLOT_SIZE          128   // 16 + 112
#define USER_FLASH_BASE         0x0000  // Data-Flash 起始地址
#define USER_EMPTY_BYTE         0xFF   // 空数据标志

// 用户数据结构（128 字节，4 字节对齐）
typedef struct {
    char     name[USER_NAME_SIZE];    // 16B 用户名
    uint8_t  data[USER_DATA_SIZE];   // 112B 用户数据
} user_record_t;  // 注意: 需确保 sizeof == 128

// API
BOOL user_flash_set(uint8_t id, const char *name, const uint8_t *data);
BOOL user_flash_get(uint8_t id, char *name, uint8_t *data);
BOOL user_flash_delete(uint8_t id);
BOOL user_flash_clear_all(void);
BOOL user_flash_is_empty(uint8_t id);
uint8_t user_flash_count(void);
void user_flash_init(void);
```

### 2. 新增 `APP/user_flash.c`

核心实现逻辑：

**user\_flash\_set(id, name, data):**

1. 检查 id < 50
2. 组装 user\_record\_t（name 不足 16B 补 0，data 不足 112B 补 0）
3. 计算地址 `addr = id * 128`
4. 读取包含该地址的 256 字节页到临时缓冲
5. 将 user\_record\_t 写入缓冲中对应 128 字节位置
6. 擦除该 256 字节页
7. 写回整个 256 字节页

**user\_flash\_get(id, name, data):**

1. 检查 id < 50
2. `EEPROM_READ(id * 128, &record, 128)`
3. 检查是否为空（name\[0] == 0xFF && data\[0] == 0xFF）
4. 复制 name 和 data 到输出参数

**user\_flash\_delete(id):**

1. 读取包含该用户的 256 字节页
2. 将目标 128 字节填充为 0xFF
3. 擦除 256 字节页
4. 写回 256 字节页

**user\_flash\_clear\_all():**

1. 从地址 0 开始，每次擦除 4096 字节（block），擦除 2 个 block = 8192 字节覆盖全部 6400 字节

   * 或按 256 字节页擦除，共 25 页（6400/256≈25，向上取整 26 页 = 6656 字节）

**user\_flash\_is\_empty(id):**

1. 读取前 2 字节（name\[0] 和 data\[0]）
2. 如果都是 0xFF → 空

**user\_flash\_count():**

1. 遍历 0-49，统计非空的数量

**关键注意事项:**

* 临时缓冲区用 `static __attribute__((aligned(4)))` 确保四字节对齐

* user\_record\_t 用编译期断言确保 sizeof == 128

* EEPROM 操作返回 0 表示成功

### 3. 新增 `APP/user_flash_cli.c`（CLI 命令层）

注册 `user` 子命令到 CLI：

* `user set <id> <name> <data>` — 设置用户（name 最长 15 字符，data 为字符串）

* `user get <id>` — 读取并显示用户名和数据

* `user del <id>` — 删除指定用户

* `user clear` — 清空全部

* `user list` — 列出所有非空用户

* `user count` — 显示已用数量

* `user help` — 显示帮助

通过 `cli_register_cmds()` 注册，参考 `fp_app_cmds.c` 模式。

### 4. 修改 `APP/hidkbd_main.c`

在 `main()` 的 `Fp_Init()` 之后调用 `user_flash_init()` 和 CLI 注册（extern 声明 `user_flash_cli_init()`）。

### 5. 修改 `APP/subdir_extra.mk`

追加 `user_flash.c` / `user_flash_cli.c` 到 `_cli_srcs` / `_cli_deps` / `_cli_objs`。

## Assumptions & Decisions

1. **Data-Flash 基址**: 使用地址 0x0000，与 BLE SNV (0x7E00) 无冲突
2. **结构布局**: username(16B) + data(112B) = 128B 固定，与 ID 映射 `addr = id * 128`
3. **空判定**: name\[0]==0xFF && data\[0]==0xFF → 空槽
4. **单用户删除**: 采用读-改-写策略（256 字节页粒度），因为 EEPROM 最小擦除单位是 256 字节
5. **全清策略**: 按 4096 字节 block 擦除 2 次（8192 字节 > 6400 字节），速度快
6. **数据格式**: 用户名和用户数据均按字符串存储（0 结尾），写入时不足部分补 0x00
7. **不依赖 pt/protothread**: flash 操作是同步阻塞的（EEPROM 操作耗时在毫秒级），在 CLI 命令上下文直接调用即可
8. **CLI data 参数**: 作为字符串传入，最大 111 字符（第 112 字节为 0 结尾）

## Verification

1. 编译通过：`make all` 无 error
2. 用户数据读写 round-trip：`user set 0 alice hello` → `user get 0` 显示 `name=alice, data=hello`
3. 删除验证：`user del 0` → `user get 0` 显示空
4. 全清验证：`user clear` → `user list` 显示无数据
5. 边界测试：id=49 可正常读写，id=50 返回错误

