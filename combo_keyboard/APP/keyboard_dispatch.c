/********************************** (C) COPYRIGHT *******************************
 * File Name          : keyboard_dispatch.c
 * Description        : Unified HID keyboard dispatcher.  Abstracts the two
 *                      possible output channels (BLE HID and USB HID) behind
 *                      a single async API, with per-channel flags and a
 *                      programmable default channel.
 *******************************************************************************/
#include "keyboard_dispatch.h"
#include "hidkbd.h"
#include "usb_composite.h"

/* 当前默认通道，上电默认 BLE + USB 同时发送 */
static kbd_channel_t s_default_channel = KBD_CH_BLE;

/* 记住最近一次通过 keyboard_type_text / keyboard_type 激活的通道掩码，
 * 便于 busy()/progress() 只统计“本次被要求的那些通道”。 */
static kbd_channel_t s_active_channels = KBD_CH_NONE;

int keyboard_type_text(const char *text, kbd_channel_t ch)
{
    if (!text || ch == KBD_CH_NONE) return 0;

    int ble_scheduled = -1;
    int usb_scheduled = -1;

    if (ch & KBD_CH_BLE) {
        ble_scheduled = hidkbd_type_text(text);
    }
    if (ch & KBD_CH_USB) {
        usb_scheduled = usb_hid_send_text(text);
    }

    s_active_channels = ch;

    /* 返回成功入队的字符数（取所有请求通道的最小值；-1 表示该通道未启用） */
    if (ble_scheduled < 0 && usb_scheduled < 0) return 0;
    if (ble_scheduled < 0) return usb_scheduled;
    if (usb_scheduled < 0) return ble_scheduled;
    return (ble_scheduled < usb_scheduled) ? ble_scheduled : usb_scheduled;
}

int keyboard_type(const char *text)
{
    return keyboard_type_text(text, s_default_channel);
}

void keyboard_set_channel(kbd_channel_t ch)
{
    s_default_channel = ch;
}

kbd_channel_t keyboard_get_channel(void)
{
    return s_default_channel;
}

const char *keyboard_channel_name(kbd_channel_t ch)
{
    switch (ch) {
        case KBD_CH_NONE: return "none";
        case KBD_CH_BLE:  return "ble";
        case KBD_CH_USB:  return "usb";
        case KBD_CH_BOTH: return "both";
        default:          return "unknown";
    }
}

/* 只要任意被请求通道仍在忙，整体就视为 busy。
 * 如果 s_active_channels 仍在NONE（未调用过 type）直接返回 0 不忙。 */
int keyboard_type_busy(void)
{
    if (s_active_channels == KBD_CH_NONE) return 0;

    int any_busy = 0;
    if (s_active_channels & KBD_CH_BLE) any_busy |= hidkbd_type_busy();
    if (s_active_channels & KBD_CH_USB) any_busy |= usb_hid_type_busy();
    return any_busy;
}

/* 进度取所有被请求通道中的“较小值”，用于 CLI 展示整体打字进度。 */
int keyboard_type_progress(void)
{
    if (s_active_channels == KBD_CH_NONE) return 0;

    int ble_p = -1;
    int usb_p = -1;
    if (s_active_channels & KBD_CH_BLE) ble_p = hidkbd_type_progress();
    if (s_active_channels & KBD_CH_USB) usb_p = usb_hid_type_progress();

    if (ble_p < 0 && usb_p < 0) return 0;
    if (ble_p < 0) return usb_p;
    if (usb_p < 0) return ble_p;
    return (ble_p < usb_p) ? ble_p : usb_p;
}
