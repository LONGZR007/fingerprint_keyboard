#ifndef USB_COMPOSITE_H
#define USB_COMPOSITE_H
#include <stdint.h>

void usb_composite_init(void);
void usb_composite_task(void);
void usb_hid_send_key(uint8_t modifier, uint8_t keycode);

/*
 * Schedule a NUL-terminated ASCII string for async transmission via the
 * USB HID keyboard endpoint.  Returns the number of chars scheduled, or
 * 0 if the text is empty or a previous job is still running.
 */
int usb_hid_send_text(const char *text);

/*
 * Query whether an async usb_hid_send_text() job is in flight, and how
 * many characters have been successfully emitted so far.
 */
int usb_hid_type_busy(void);
int usb_hid_type_progress(void);

int usb_cdc_send(const uint8_t *buf, uint16_t len);
int usb_cdc_recv(uint8_t *buf, uint16_t maxlen);
uint8_t usb_cdc_available(void);
uint8_t usb_cdc_connected(void);

#endif
