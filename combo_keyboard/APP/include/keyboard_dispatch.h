#ifndef KEYBOARD_DISPATCH_H
#define KEYBOARD_DISPATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 键盘发送通道标志（可按位组合）
 * __attribute__((packed)): 强制枚举为最小宽度 (1 字节),
 * 保证 sizeof(kbd_channel_t) == 1, 使 user_record_t 恰好 128B
 * 与 Data-Flash 槽位大小 (USER_SLOT_SIZE) 一致 */
typedef enum __attribute__((packed)) {
    KBD_CH_NONE = 0x00,
    KBD_CH_BLE  = 0x01,
    KBD_CH_USB  = 0x02,
    KBD_CH_BOTH = KBD_CH_BLE | KBD_CH_USB,
} kbd_channel_t;

/*
 * 统一键盘发送接口（异步）：按 channel 标志同时向指定通道调度 ASCII
 * 字符串发送。实际按键报告在 BLE TMOS 事件 / USB 主循环 task 中推进，
 * 调用者绝不阻塞。
 *
 * 返回本次实际“成功入队”的字符数（>=0）：
 *   - 返回 0 表示：输入空 / 上一次调度仍在执行 / 所选通道无可用
 *   - 返回 >0 表示：至少一个请求通道已成功入队
 *
 * 注意：调度后可通过 keyboard_type_busy() / keyboard_type_progress()
 * 查询执行状态；两通道的执行节奏不同，最终 progress 为两者较小值。
 */
int  keyboard_type_text(const char *text, kbd_channel_t ch);

/* 使用当前默认通道发送（由 keyboard_set_channel 设置）。 */
int  keyboard_type(const char *text);

/* 设置 / 获取当前默认发送通道。 */
void keyboard_set_channel(kbd_channel_t ch);
kbd_channel_t keyboard_get_channel(void);

/* 所有启用通道都发送完成时返回 0，否则返回 1。 */
int  keyboard_type_busy(void);

/* 返回当前正在发送的字符串中，已成功发出的字符数（以进度较慢的通道为准）。 */
int  keyboard_type_progress(void);

/* 通道名称转换（用于 CLI 显示）。 */
const char *keyboard_channel_name(kbd_channel_t ch);

#ifdef __cplusplus
}
#endif

#endif
