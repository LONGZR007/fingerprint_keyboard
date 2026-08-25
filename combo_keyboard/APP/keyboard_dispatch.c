/********************************** (C) COPYRIGHT *******************************
 * File Name          : keyboard_dispatch.c
 * Description        : 统一键盘发送调度层，通过通道标志选择 BLE HID / USB HID / 两者。
 *******************************************************************************/
#include "keyboard_dispatch.h"
#include "hidkbd.h"
#include "usb_composite.h"

/* 当前默认通道，上电默认 BLE + USB 同时发送 */
static kbd_channel_t s_default_channel = KBD_CH_BOTH;

int keyboard_type_text(const char *text, kbd_channel_t ch)
{
    if (!text || ch == KBD_CH_NONE) return 0;

    int ble_ret = -1;
    int usb_ret = -1;

    if (ch & KBD_CH_BLE) {
        ble_ret = hidkbd_type_text(text);
    }
    if (ch & KBD_CH_USB) {
        usb_ret = usb_hid_send_text(text);
    }

    /* 返回较小值（未发送的通道返回 -1 不影响） */
    if (ble_ret < 0 && usb_ret < 0) return 0;
    if (ble_ret < 0) return usb_ret;
    if (usb_ret < 0) return ble_ret;
    return (ble_ret < usb_ret) ? ble_ret : usb_ret;
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
