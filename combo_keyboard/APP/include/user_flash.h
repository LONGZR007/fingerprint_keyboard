/********************************** (C) COPYRIGHT *******************************
 * File Name          : user_flash.h
 * Description        : Data-Flash 用户数据存储接口
 *                      基于 CH583 Data-Flash (EEPROM)，固定槽位映射：
 *                        id0 -> 偏移 0, id1 -> 偏移 128, id_n -> 偏移 n*128
 *                      每用户 128 字节 = 用户名 16B + 用户数据 111B + 上报通道 1B
 *                      空数据判定：用户名首字节 == 0xFF 且 用户数据首字节 == 0xFF
 *                      清除即把槽位恢复为 0xFF（依赖 flash 擦除）
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 *******************************************************************************/

#ifndef __USER_FLASH_H__
#define __USER_FLASH_H__

#include "CH58x_common.h"   /* EEPROM_READ/WRITE/ERASE, BOOL 等 */
#include "keyboard_dispatch.h"  /* kbd_channel_t 上报通道类型 */

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * 配置宏
 * ------------------------------------------------------------------------ */
#ifndef USER_FLASH_MAX_USERS
#define USER_FLASH_MAX_USERS    50      /* 最多 50 个用户                      */
#endif
#ifndef USER_NAME_SIZE
#define USER_NAME_SIZE          16      /* 用户名 16 字节                      */
#endif
#ifndef USER_DATA_SIZE
#define USER_DATA_SIZE          111     /* 用户数据 111 字节                   */
#endif
#define USER_CHANNEL_OFFSET     (USER_NAME_SIZE + USER_DATA_SIZE)  /* 上报通道偏移: 记录末尾 1B */
#define USER_SLOT_SIZE          (USER_NAME_SIZE + USER_DATA_SIZE + 1)  /* 128 */
#define USER_FLASH_BASE         0x0000  /* Data-Flash 起始偏移                 */
#define USER_EMPTY_BYTE         0xFF    /* 空数据标志                         */
#define USER_FLASH_PAGE_SIZE    256     /* Data-Flash 擦除页大小 (2 槽位/页)   */

/* 用户区总大小: 50 * 128 = 6400 字节 (偏移 0 ~ 6399)
 * BLE_SNV 占偏移 0x7E00(32256) ~ 0x7FFF, 与用户区无重叠 */

/* --------------------------------------------------------------------------
 * 数据结构 (仅作类型参考, sizeof == 128)
 * ------------------------------------------------------------------------ */
typedef struct {
    char         name[USER_NAME_SIZE];    /* 16B 用户名 (以 \0 结尾字符串)   */
    uint8_t      data[USER_DATA_SIZE];   /* 111B 用户数据 (以 \0 结尾字符串) */
    kbd_channel_t channel;               /* 1B  上报通道 (kbd_channel_t 值)  */
} user_record_t;

/* 编译期校验槽位大小 */
#ifndef __STATIC_ASSERT_USER_RECORD
#define __STATIC_ASSERT_USER_RECORD
#endif

/* --------------------------------------------------------------------------
 * API
 *  返回 BOOL: TRUE=成功, FALSE=失败 (含读错误/参数非法/空槽)
 * ------------------------------------------------------------------------ */

/* 初始化 (当前为空操作, 不擦除以免清空数据; 出厂态默认 0xFF) */
void    user_flash_init(void);

/* 设置用户数据: name/data 均为以 \0 结尾字符串, 内部零填充到 16/111 字节;
 * ch 指定上报通道 (kbd_channel_t, 仅保留 BLE/USB 位) */
BOOL    user_flash_set(uint8_t id, const char *name, const uint8_t *data, kbd_channel_t ch);

/* 读取用户数据: 空槽返回 FALSE 且不拷贝; 非空拷贝并保证 name/data 以 \0 结尾,
 * 同时通过 *ch 返回存储的上报通道 (仅保留 BLE/USB 位) */
BOOL    user_flash_get(uint8_t id, char *name, uint8_t *data, kbd_channel_t *ch);

/* 删除指定用户: 把该槽位恢复为 0xFF */
BOOL    user_flash_delete(uint8_t id);

/* 清空全部用户: 擦除整个用户区 (仅 0~6399, 不触碰 SNV) */
BOOL    user_flash_clear_all(void);

/* 槽位是否为空 (读失败按空处理) */
BOOL    user_flash_is_empty(uint8_t id);

/* 统计非空用户数 (0 ~ USER_FLASH_MAX_USERS) */
uint8_t user_flash_count(void);

#ifdef __cplusplus
}
#endif

#endif /* __USER_FLASH_H__ */
