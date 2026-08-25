#ifndef KEYBOARD_DISPATCH_H
#define KEYBOARD_DISPATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 键盘发送通道标志（可按位组合） */
typedef enum {
    KBD_CH_NONE = 0x00,
    KBD_CH_BLE  = 0x01,
    KBD_CH_USB  = 0x02,
    KBD_CH_BOTH = KBD_CH_BLE | KBD_CH_USB,
} kbd_channel_t;

/*
 * 统一键盘发送接口：按 channel 标志同时向指定通道发送 ASCII 字符串。
 * 返回成功发送的字符数（取两个通道中较小值，因为不同通道可能跳过不同字符）。
 */
int keyboard_type_text(const char *text, kbd_channel_t ch);

/*
 * 使用当前默认通道发送（由 keyboard_set_channel 设置）。
 */
int keyboard_type(const char *text);

/*
 * 设置 / 获取当前默认发送通道。
 */
void keyboard_set_channel(kbd_channel_t ch);
kbd_channel_t keyboard_get_channel(void);

/*
 * 通道名称转换（用于 CLI 显示）。
 */
const char *keyboard_channel_name(kbd_channel_t ch);

#ifdef __cplusplus
}
#endif

#endif
