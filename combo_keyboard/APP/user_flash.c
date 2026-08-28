/********************************** (C) COPYRIGHT *******************************
 * File Name          : user_flash.c
 * Description        : Data-Flash 用户数据存储接口实现
 *
 * 关键设计 (见 .trae/documents/user-flash-storage.md):
 *   - Data-Flash 偏移地址: id_n -> n * 128, 用户区 0 ~ 6399 (50 用户)
 *   - 擦除粒度 256B > 槽位 128B, 且写只能 1->0, 故 set/delete 走页级
 *     "读整页 -> 改目标槽 -> 擦页 -> 写回整页" (邻居槽原样恢复)
 *   - 空判定: name[0]==0xFF && data[0]==0xFF
 *   - BLE_SNV 在偏移 0x7E00, 与用户区无重叠
 *   - EEPROM_* 要求 buffer 4 字节对齐
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 *******************************************************************************/

#include "user_flash.h"
#include <string.h>

/* 编译期校验: 槽位必须 128 字节 */
typedef char _user_rec_size_check[1 - 2 * !(sizeof(user_record_t) == USER_SLOT_SIZE)];

/* =======================================================================
 * 私有 4 字节对齐缓冲 (CLI 单线程 TMOS 调用, 无重入风险)
 * ===================================================================== */
static __attribute__((aligned(4))) uint8_t s_page_buf[USER_FLASH_PAGE_SIZE]; /* 256B 页 RMW */
static __attribute__((aligned(4))) uint8_t s_rec_buf[USER_SLOT_SIZE];        /* 128B 单记录 */

/* id -> 偏移 */
static inline uint32_t user_flash_offset(uint8_t id)
{
    return (uint32_t)id * USER_SLOT_SIZE;
}

/* 空数据判定: 用户名首字节 == 0xFF 且 用户数据首字节 == 0xFF */
static BOOL user_rec_is_empty(const uint8_t *rec)
{
    return (rec[0] == USER_EMPTY_BYTE) &&
           (rec[USER_NAME_SIZE] == USER_EMPTY_BYTE);
}

/* =======================================================================
 * 页级 Read-Modify-Write (set/delete 共用)
 *   new_rec != NULL: 把目标槽写成 new_rec (128B)
 *   new_rec == NULL: 把目标槽写成 0xFF (删除)
 *   邻居槽原样恢复
 * ===================================================================== */
static BOOL user_flash_page_rmw(uint8_t id, const uint8_t *new_rec)
{
    uint8_t  page_id = (uint8_t)(id / 2);
    uint8_t  slot    = (uint8_t)(id & 1);          /* 0 或 1 */
    uint32_t page_off = (uint32_t)page_id * USER_FLASH_PAGE_SIZE;
    uint8_t *slot_ptr = s_page_buf + (uint32_t)slot * USER_SLOT_SIZE;

    /* 1. 读整页 */
    if (EEPROM_READ(page_off, s_page_buf, USER_FLASH_PAGE_SIZE)) {
        return FALSE;
    }

    /* 2. 改目标槽 */
    if (new_rec) {
        memcpy(slot_ptr, new_rec, USER_SLOT_SIZE);
    } else {
        memset(slot_ptr, USER_EMPTY_BYTE, USER_SLOT_SIZE);
    }

    /* 3. 擦整页 (256B) */
    if (EEPROM_ERASE(page_off, USER_FLASH_PAGE_SIZE)) {
        return FALSE;
    }

    /* 4. 写回整页 (邻居槽原样恢复) */
    if (EEPROM_WRITE(page_off, s_page_buf, USER_FLASH_PAGE_SIZE)) {
        return FALSE;
    }
    return TRUE;
}

/* =======================================================================
 * 公共 API
 * ===================================================================== */
void user_flash_init(void)
{
    /* 当前为空操作: 不擦除以免清空已有用户数据
     * Data-Flash 出厂态默认 0xFF, 空判定天然兼容;
     * 需格式化时由 CLI "user clear" 完成 */
}

BOOL user_flash_set(uint8_t id, const char *name, const uint8_t *data, kbd_channel_t ch)
{
    if (id >= USER_FLASH_MAX_USERS) {
        return FALSE;
    }
    if (name == NULL || data == NULL) {
        return FALSE;
    }

    /* 装配 128 字节记录, 全部零填充 (零 != 0xFF, 不会被误判为空) */
    memset(s_rec_buf, 0, USER_SLOT_SIZE);
    /* 用户名: 复制最多 16 字节, strncpy 自动用 \0 填充不足部分 */
    strncpy((char *)s_rec_buf, name, USER_NAME_SIZE);
    /* 用户数据: 复制最多 111 字节, 字符串方式 (遇 \0 停止, 避免越界读源) */
    strncpy((char *)(s_rec_buf + USER_NAME_SIZE), (const char *)data, USER_DATA_SIZE);
    /* 上报通道: 存于记录末尾 1 字节, 仅保留 BLE/USB 位 */
    s_rec_buf[USER_CHANNEL_OFFSET] = (uint8_t)(ch & KBD_CH_BOTH);

    return user_flash_page_rmw(id, s_rec_buf);
}

BOOL user_flash_get(uint8_t id, char *name, uint8_t *data, kbd_channel_t *ch)
{
    if (id >= USER_FLASH_MAX_USERS) {
        return FALSE;
    }

    /* 读 128 字节记录到对齐缓冲 */
    if (EEPROM_READ(user_flash_offset(id), s_rec_buf, USER_SLOT_SIZE)) {
        return FALSE;
    }

    /* 空槽: 不拷贝 */
    if (user_rec_is_empty(s_rec_buf)) {
        return FALSE;
    }

    /* 非空: 拷贝并保证以 \0 结尾 */
    if (name) {
        memcpy(name, s_rec_buf, USER_NAME_SIZE);
        name[USER_NAME_SIZE - 1] = '\0';
    }
    if (data) {
        memcpy(data, s_rec_buf + USER_NAME_SIZE, USER_DATA_SIZE);
        data[USER_DATA_SIZE - 1] = '\0';
    }
    if (ch) {
        /* 仅保留 BLE/USB 位; 旧数据/未设置时该字节可能为 0xFF 或 0, 由调用方回退默认 */
        *ch = (kbd_channel_t)(s_rec_buf[USER_CHANNEL_OFFSET] & KBD_CH_BOTH);
    }
    return TRUE;
}

BOOL user_flash_delete(uint8_t id)
{
    if (id >= USER_FLASH_MAX_USERS) {
        return FALSE;
    }
    /* 把目标槽置 0xFF (依赖擦除) */
    return user_flash_page_rmw(id, NULL);
}

BOOL user_flash_clear_all(void)
{
    /* 擦除整个用户区: 25 页 (偏移 0 ~ 6399), 仅用户区, 不触碰 SNV */
    uint8_t pages = (uint8_t)((USER_FLASH_MAX_USERS + 1) / 2);  /* 25 页 */
    for (uint8_t p = 0; p < pages; p++) {
        uint32_t page_off = (uint32_t)p * USER_FLASH_PAGE_SIZE;
        if (EEPROM_ERASE(page_off, USER_FLASH_PAGE_SIZE)) {
            return FALSE;
        }
    }
    return TRUE;
}

BOOL user_flash_is_empty(uint8_t id)
{
    if (id >= USER_FLASH_MAX_USERS) {
        return TRUE;
    }
    /* 读失败按空处理 (安全默认) */
    if (EEPROM_READ(user_flash_offset(id), s_rec_buf, USER_SLOT_SIZE)) {
        return TRUE;
    }
    return user_rec_is_empty(s_rec_buf);
}

uint8_t user_flash_count(void)
{
    uint8_t n = 0;
    for (uint8_t id = 0; id < USER_FLASH_MAX_USERS; id++) {
        if (!user_flash_is_empty(id)) {
            n++;
        }
    }
    return n;
}
