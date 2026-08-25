#ifndef USB_COMPOSITE_H
#define USB_COMPOSITE_H
#include <stdint.h>

void usb_composite_init(void);
void usb_composite_task(void);
void usb_hid_send_key(uint8_t modifier, uint8_t keycode);
int usb_hid_send_text(const char *text);
int usb_cdc_send(const uint8_t *buf, uint16_t len);
int usb_cdc_recv(uint8_t *buf, uint16_t maxlen);
uint8_t usb_cdc_available(void);
uint8_t usb_cdc_connected(void);

#endif
