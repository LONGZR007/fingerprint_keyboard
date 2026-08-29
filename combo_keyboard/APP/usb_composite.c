/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_composite.c
 * Description        : CH583 USB composite device (HID keyboard + CDC ACM serial)
 *                      Implemented on the first USB peripheral (R8_USB_* / R8_UEP*).
 *********************************************************************************
 * Endpoint allocation:
 *   EP0  Control          (shared, 64B)   - setup + EP4 DMA area
 *   EP1  HID Keyboard IN  (Interrupt, 8B)
 *   EP2  CDC Notify IN     (Interrupt, 8B)
 *   EP3  CDC Data IN       (Bulk, 64B)
 *   EP4  CDC Data OUT      (Bulk, 64B, shares DMA buffer with EP0)
 *******************************************************************************/

#include "CH58x_common.h"
#include "CH58xBLE_LIB.h"      /* TMOS_GetSystemClock() ms counter */
#include "usb_composite.h"
#include "cli.h"
#include "cli_uart.h"          /* cli_uart_putc(): UART1-only debug out */
#include <string.h>

/* USB enumeration debug log via UART1 (CLI channel).  Define to 0 to disable. */
#define USB_DEBUG_LOG 1
#if USB_DEBUG_LOG
#define USB_LOG(...)  cli_print(__VA_ARGS__)
#else
#define USB_LOG(...)  ((void)0)
#endif

/*********************************************************************
 * DMA buffers (4-byte aligned)
 *   EP0_Databuf: ep0(64) + ep4_out(64) + ep4_in(64)
 *   EP1_Databuf: ep1_out(64) + ep1_in(64)
 *   EP2_Databuf: ep2_out(64) + ep2_in(64)
 *   EP3_Databuf: ep3_out(64) + ep3_in(64)
 *********************************************************************/
__attribute__((aligned(4))) static uint8_t EP0_Databuf[64 + 64 + 64];
__attribute__((aligned(4))) static uint8_t EP1_Databuf[64 + 64];
__attribute__((aligned(4))) static uint8_t EP2_Databuf[64 + 64];
__attribute__((aligned(4))) static uint8_t EP3_Databuf[64 + 64];

#define Ep0Buffer    (&EP0_Databuf[0])      /* EP0 setup/data          */
#define Ep4OutBuffer (&EP0_Databuf[64])     /* EP4 OUT data (CDC RX)  */
#define Ep1InBuffer  (&EP1_Databuf[0])      /* EP1 IN  (HID reports)  */
#define Ep2InBuffer  (&EP2_Databuf[0])      /* EP2 IN  (CDC notify)   */
#define Ep3InBuffer  (&EP3_Databuf[0])      /* EP3 IN  (CDC TX)       */

/*********************************************************************
 * Types
 *********************************************************************/
typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_req_t;

typedef struct {
    uint32_t baud_rate;
    uint8_t  stop_bits;
    uint8_t  parity;
    uint8_t  data_bits;
} line_coding_t;

/*********************************************************************
 * Descriptors
 *********************************************************************/

/* Device descriptor (18B): IAD composite, VID=0x413D, PID=0x2107 */
static const uint8_t DevDesc[18] = {
    0x12, 0x01, 0x10, 0x01, 0xEF, 0x02, 0x01, 0x40,
    0x3d, 0x41, 0x07, 0x21, 0x00, 0x00, 0x01, 0x02, 0x03, 0x01
};

/* Configuration descriptor (100B, 3 interfaces: HID + CDC-Comm + CDC-Data).
 * NOTE: wTotalLength below is set to 0x64 (100) to match the actual byte
 *       content so the host can see the CDC Data interface's EP4 OUT endpoint.
 *       (The original spec text cited 0x5D/93, which omitted the IAD/EP4 size.) */
static const uint8_t CfgDesc[] = {
    /* Config descriptor */
    0x09, 0x02, 0x64, 0x00, 0x03, 0x01, 0x00, 0x80, 0x32,
    /* Interface 0: HID Keyboard */
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3E, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0A,
    /* Interface 1: CDC Communication (IAD) */
    0x08, 0x0B, 0x01, 0x02, 0x02, 0x02, 0x00, 0x00,
    0x09, 0x04, 0x01, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,
    0x05, 0x24, 0x00, 0x10, 0x01,
    0x04, 0x24, 0x02, 0x02,
    0x05, 0x24, 0x06, 0x01, 0x02,
    0x05, 0x24, 0x01, 0x01, 0x02,
    0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0x01,
    /* Interface 2: CDC Data */
    0x09, 0x04, 0x02, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x83, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x04, 0x02, 0x40, 0x00, 0x00
};

/* HID keyboard report descriptor (62B) - from CompoundDev KeyRepDesc */
static const uint8_t HidRepDesc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07,
    0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0x95, 0x03, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x03, 0x91, 0x02,
    0x95, 0x05, 0x75, 0x01, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x26, 0xff, 0x00, 0x05, 0x07, 0x19,
    0x00, 0x29, 0x91, 0x81, 0x00, 0xC0
};

/* HID class descriptor is embedded in CfgDesc at offset 18 (9 bytes) */
#define HID_DESC_OFFSET 18

/* String descriptors */
static const uint8_t LangDesc[]   = { 0x04, 0x03, 0x09, 0x04 };
static const uint8_t ManuDesc[]   = { 0x0E, 0x03, 'w', 0, 'c', 0, 'h', 0, '.', 0, 'c', 0, 'n', 0 };
static const uint8_t ProdDesc[]   = { 0x14, 0x03, 'C', 0, 'o', 0, 'm', 0, 'b', 0, 'o', 0, 'K', 0, 'e', 0, 'y', 0, 'b', 0 };
static const uint8_t SerialDesc[] = { 0x0C, 0x03, '0', 0, '0', 0, '1', 0, '0', 0, '1', 0 };

static const uint8_t *const StringDesc[] = { LangDesc, ManuDesc, ProdDesc, SerialDesc };
#define STRING_DESC_COUNT (sizeof(StringDesc) / sizeof(StringDesc[0]))

/*********************************************************************
 * State
 *********************************************************************/
static volatile uint8_t  usb_setup_flag;          /* SETUP received, needs task processing */
static volatile uint8_t  usb_dev_config;          /* non-zero once SET_CONFIGURATION done   */
static volatile uint8_t  usb_suspended;
static volatile uint8_t  usb_bus_reset_pending;   /* printed by task for diagnostics       */

static volatile uint8_t  ep1_in_ready;            /* EP1 IN (HID) transfer complete          */
static volatile uint8_t  ep3_in_ready;            /* EP3 IN (CDC TX) transfer complete       */

static volatile uint8_t  pending_addr;           /* address to apply after SET_ADDRESS ZLP  */
static volatile uint8_t  addr_pending;

static volatile uint8_t  ep0_out_expect_line;    /* expecting SET_LINE_CODING OUT data     */
static volatile uint8_t  cdc_line_state;         /* DTR/RTS bitmap from SET_CONTROL_LINE    */

static line_coding_t line_coding = { 115200, 0, 0, 8 };

/* EP0 control-IN data phase tracking */
static const uint8_t *ep0_in_ptr;
static uint16_t       ep0_in_remaining;
static uint8_t        ep0_in_zlp;

#define CDC_RX_RING_SIZE 256
static uint8_t         cdc_rx_ring[CDC_RX_RING_SIZE];
static volatile uint16_t cdc_rx_head;      /* written by EP4 IRQ path    */
static volatile uint16_t cdc_rx_tail;      /* consumed by usb_cdc_recv() */

/* ---- CDC TX: non-blocking ring, flushed from usb_composite_task() ----
 * usb_cdc_send() only enqueues (never blocks), so the CLI can mirror its
 * output to the USB CDC port without stalling on EP3 flow. */
#define CDC_TX_RING_SIZE 1024
static uint8_t          cdc_tx_ring[CDC_TX_RING_SIZE];
static volatile uint16_t cdc_tx_head;
static volatile uint16_t cdc_tx_tail;

static void cdc_tx_pump(void)
{
    uint16_t tail = cdc_tx_tail;
    uint16_t head = cdc_tx_head;
    uint16_t n = 0;

    if (tail == head) return;                 /* nothing queued        */
    if (!ep3_in_ready) return;                /* previous frame in air */
    while (n < 64 && tail != head) {
        Ep3InBuffer[n++] = cdc_tx_ring[tail];
        if (++tail >= CDC_TX_RING_SIZE) tail = 0;
    }
    cdc_tx_tail = tail;
    ep3_in_ready = 0;
    R8_UEP3_T_LEN = (uint8_t)n;
    R8_UEP3_CTRL = (R8_UEP3_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
    /* RB_UEP_AUTO_TOG: hardware flips the TX toggle after this transfer
     * completes, so the next frame only needs T_LEN + T_RES_ACK again.
     * The IRQ restores T_RES=NAK on completion, so no stale re-send. */
}

/* ---- Async USB HID string-type state machine ----
 *
 * 与 BLE hidkbd_type_text 状态机一致：
 *   PRESS -> WAIT_PRESS -> RELEASE -> WAIT_RELEASE -> NEXT -> PRESS
 * 文本在入队时被 usb_hid_parse 预解析为 HID 按键序列
 * （普通字符一对一、"enter"/"esc" 等特殊键、\" 转义），
 * 状态机只消费预解析好的 (modifier, keycode)。
 * 每次 usb_composite_task() 调用最多推进一步，绝不阻塞。
 * 两个 WAIT 阶段靠系统时钟轮询 ~1ms 间隔（全速 USB 典型 bulk 节奏）。
 * 不使用 busy-loop 的 DelayUs，避免抢占 BLE 调度。 */
#define USB_TYPE_BUF_SIZE  128
#define USB_STEP_PRESS     0
#define USB_STEP_WAIT_PRESS 1
#define USB_STEP_RELEASE   2
#define USB_STEP_WAIT_REL  3
#define USB_STEP_NEXT      4
#define USB_STEP_DELAY_MS  25UL   /* press/release hold time, ms */

/* 预解析的按键序列项: (HID modifier, HID keycode) */
typedef struct {
    uint8_t mod;
    uint8_t kc;
} usb_kbd_seq_key_t;

/* 特殊键名表: "enter" -> 回车, "esc" -> ESC ... 键名大小写不敏感 */
typedef struct {
    const char *name;
    uint8_t     keycode;
} usb_hid_special_key_t;

static const usb_hid_special_key_t s_usb_special_keys[] = {
    { "enter",     0x28 },  /* Enter / Return      */
    { "esc",       0x29 },  /* Escape              */
    { "backspace", 0x2A },  /* Backspace           */
    { "tab",       0x2B },  /* Tab                 */
    { "space",     0x2C },  /* Space               */
    { "capslock",  0x39 },  /* Caps Lock           */
    { "insert",    0x49 },  /* Insert              */
    { "home",      0x4A },  /* Home                */
    { "pageup",    0x4B },  /* Page Up             */
    { "delete",    0x4C },  /* Delete (Del)        */
    { "end",       0x4D },  /* End                 */
    { "pagedown",  0x4E },  /* Page Down           */
    { "right",     0x4F },  /* Right Arrow         */
    { "left",      0x50 },  /* Left Arrow          */
    { "down",      0x51 },  /* Down Arrow          */
    { "up",        0x52 },  /* Up Arrow            */
    { "f1",        0x3A },  /* F1 ~ F12            */
    { "f2",        0x3B },
    { "f3",        0x3C },
    { "f4",        0x3D },
    { "f5",        0x3E },
    { "f6",        0x3F },
    { "f7",        0x40 },
    { "f8",        0x41 },
    { "f9",        0x42 },
    { "f10",       0x43 },
    { "f11",       0x44 },
    { "f12",       0x45 },
};

#define USB_SPECIAL_KEY_NUM (sizeof(s_usb_special_keys) / sizeof(s_usb_special_keys[0]))

static usb_kbd_seq_key_t s_ut_seq[USB_TYPE_BUF_SIZE];  /* 入队时预解析的按键序列 */
static uint16_t s_ut_len    = 0;   /* 按键数（解析后） */
static uint16_t s_ut_idx    = 0;
static uint8_t  s_ut_step   = USB_STEP_PRESS;
static volatile uint8_t s_ut_busy = 0;
static int       s_ut_typed = 0;
static uint32_t  s_ut_deadline = 0;   /* ms deadline via TMOS_GetSystemClock */

static int usb_ascii_to_hid(char ch, uint8_t *modifier, uint8_t *keycode);

/*********************************************************************
 * Deferred IRQ event ring buffer (single producer: IRQ, single consumer: task)
 *********************************************************************/
#define IRQ_RING_SIZE 32
typedef struct {
    uint8_t token;   /* UIS_TOKEN_IN / UIS_TOKEN_OUT */
    uint8_t ep;      /* endpoint index (0..4)         */
    uint8_t len;     /* R8_USB_RX_LEN for OUT         */
    uint8_t tog_ok;  /* RB_UIS_TOG_OK at transfer time */
} irq_evt_t;

static volatile irq_evt_t irq_ring[IRQ_RING_SIZE];
static volatile uint8_t   irq_head;
static volatile uint8_t   irq_tail;
static volatile uint16_t  usb_evt_dropped;   /* ring overflow counter  */
static volatile uint32_t  usb_transfer_irq_cnt; /* total TRANSFER irq  */
static volatile uint32_t  usb_setup_irq_cnt;     /* SETUP_ACT irq count */
static volatile uint32_t  usb_ep1in_irq, usb_ep3in_irq, usb_ep4out_irq;

static uint8_t irq_ring_pop(irq_evt_t *e)
{
    uint8_t h = irq_head;
    uint8_t t = irq_tail;
    if (h == t) {
        return 0;
    }
    e->token  = irq_ring[t].token;
    e->ep     = irq_ring[t].ep;
    e->len    = irq_ring[t].len;
    e->tog_ok = irq_ring[t].tog_ok;
    t++;
    if (t >= IRQ_RING_SIZE) t = 0;
    irq_tail = t;
    return 1;
}

/*********************************************************************
 * EP0 control-IN helpers
 *********************************************************************/

/* Send the next chunk (<=64B) of a control-IN data phase, or the status ZLP,
 * or restore EP0 to idle when the transfer is finished.
 *
 * EP0 data toggle is NOT handled by hardware on CH58x (no RB_UEP_AUTO_TOG for
 * EP0), so the firmware must set the PID toggle itself.  After a SETUP token
 * the hardware resets the toggle to DATA0; the first data packet must then be
 * DATA1, and every later packet alternates (see the WCH CH583 examples:
 * "默认数据包是DATA1").
 *
 *  first - 1: first packet after SETUP (force DATA1)
 *          0: continuation packet (flip the DATA toggle first) */
static void ep0_send_chunk(uint8_t first)
{
    uint16_t n;

    if (ep0_in_remaining > 0) {
        n = (ep0_in_remaining > 64) ? 64 : ep0_in_remaining;
        for (uint16_t i = 0; i < n; i++) {
            Ep0Buffer[i] = ep0_in_ptr[i];
        }
        ep0_in_ptr     += n;
        ep0_in_remaining = (uint16_t)(ep0_in_remaining - n);
        R8_UEP0_T_LEN  = (uint8_t)n;
        if (first) {
            R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
        } else {
            R8_UEP0_CTRL ^= RB_UEP_T_TOG;
            R8_UEP0_CTRL = (R8_UEP0_CTRL & ~(MASK_UEP_R_RES | MASK_UEP_T_RES))
                           | UEP_R_RES_ACK | UEP_T_RES_ACK;
        }
        return;
    }

    if (ep0_in_zlp) {
        ep0_in_zlp   = 0;
        R8_UEP0_T_LEN = 0;
        if (first) {
            R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
        } else {
            R8_UEP0_CTRL ^= RB_UEP_T_TOG;
            R8_UEP0_CTRL = (R8_UEP0_CTRL & ~(MASK_UEP_R_RES | MASK_UEP_T_RES))
                           | UEP_R_RES_ACK | UEP_T_RES_ACK;
        }
        return;
    }

    /* transfer finished: idle, wait for the next SETUP (like WCH's default
     * branch).  Keep the DATA1 toggle flags so the following status-stage
     * OUT of a control read still matches. */
    R8_UEP0_T_LEN = 0;
    R8_UEP0_CTRL  = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_NAK | UEP_T_RES_NAK;
}

/* Start a control-IN data phase from a descriptor/buffer.
 *  data    - pointer to the bytes to send
 *  len     - total bytes available
 *  wLength - wLength field from the SETUP request */
static void ep0_start_in(const uint8_t *data, uint16_t len, uint16_t wLength)
{
    uint16_t n = (len < wLength) ? len : wLength;

    ep0_in_ptr      = data;
    ep0_in_remaining = n;
    /* ZLP needed only when we send a multiple of EP0 max packet (64)
     * but less than the host requested (short packet would otherwise be missing) */
    ep0_in_zlp = (n < wLength && (n % 64) == 0 && n != 0) ? 1 : 0;
    ep0_send_chunk(1);   /* first packet after SETUP must be DATA1 */
}

/* Send a zero-length status packet (control-write status phase) */
static void ep0_send_zlp(void)
{
    ep0_in_remaining = 0;
    ep0_in_zlp       = 0;
    R8_UEP0_T_LEN    = 0;
    /* status phase packet is DATA1 as well */
    R8_UEP0_CTRL     = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
}

/* Stall EP0 (unsupported request) */
static void ep0_stall(void)
{
    ep0_in_remaining = 0;
    ep0_in_zlp       = 0;
    R8_UEP0_CTRL     = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
}

/*********************************************************************
 * Setup request processing (deferred from IRQ)
 *********************************************************************/
static void usb_process_setup(void)
{
    /* Copy the SETUP packet out of Ep0Buffer first: the data phase of the
     * control transfer reuses the same DMA buffer, and the hardware writes a
     * new SETUP packet into Ep0Buffer as soon as it is accepted. */
    uint8_t setup_buf[8];
    memcpy(setup_buf, Ep0Buffer, 8);
    usb_setup_req_t *req = (usb_setup_req_t *)setup_buf;
    uint8_t  reqType = req->bmRequestType;
    uint8_t  reqCode = req->bRequest;
    uint16_t wValue  = req->wValue;
    uint16_t wIndex  = req->wIndex;
    uint16_t wLength = req->wLength;
    uint8_t  type;
    uint8_t  idx;

    /* fresh control transfer */
    ep0_in_remaining   = 0;
    ep0_in_zlp         = 0;
    ep0_out_expect_line = 0;

    USB_LOG("[USB] S t=%02X r=%02X v=%04X i=%04X l=%u\r\n",
            (unsigned)reqType, (unsigned)reqCode, (unsigned)wValue,
            (unsigned)wIndex, (unsigned)wLength);

    if ((reqType & USB_REQ_TYP_MASK) == USB_REQ_TYP_STANDARD) {
        switch (reqCode) {
        case USB_GET_DESCRIPTOR:
            type = (uint8_t)(wValue >> 8);
            idx  = (uint8_t)(wValue & 0xFF);
            if (type == USB_DESCR_TYP_DEVICE) {
                ep0_start_in(DevDesc, sizeof(DevDesc), wLength);
            } else if (type == USB_DESCR_TYP_CONFIG) {
                ep0_start_in(CfgDesc, sizeof(CfgDesc), wLength);
            } else if (type == USB_DESCR_TYP_STRING) {
                if (idx < STRING_DESC_COUNT) {
                    ep0_start_in(StringDesc[idx], StringDesc[idx][0], wLength);
                } else {
                    ep0_stall();
                }
            } else if (type == USB_DESCR_TYP_REPORT) {
                /* HID report descriptor requested for interface 0 */
                if (wIndex == 0) {
                    ep0_start_in(HidRepDesc, sizeof(HidRepDesc), wLength);
                } else {
                    ep0_stall();
                }
            } else if (type == USB_DESCR_TYP_HID) {
                /* HID class descriptor (embedded in config descriptor) */
                if (wIndex == 0) {
                    ep0_start_in(&CfgDesc[HID_DESC_OFFSET], 9, wLength);
                } else {
                    ep0_stall();
                }
            } else {
                ep0_stall();
            }
            break;

        case USB_SET_ADDRESS:
            pending_addr  = (uint8_t)(wValue & 0xFF);
            addr_pending  = 1;
            ep0_send_zlp();
            break;

        case USB_SET_CONFIGURATION:
            usb_dev_config = (uint8_t)(wValue & 0xFF);
            ep0_send_zlp();
            break;

        case USB_GET_CONFIGURATION:
            Ep0Buffer[0] = usb_dev_config;
            ep0_start_in(Ep0Buffer, 1, wLength);
            break;

        case USB_GET_STATUS:
            Ep0Buffer[0] = 0;
            Ep0Buffer[1] = 0;
            ep0_start_in(Ep0Buffer, 2, wLength);
            break;

        case USB_CLEAR_FEATURE:
        case USB_SET_FEATURE:
            ep0_send_zlp();
            break;

        default:
            ep0_stall();
            break;
        }
    } else if ((reqType & USB_REQ_TYP_MASK) == USB_REQ_TYP_CLASS) {
        /* CDC class requests (target interface 1) */
        switch (reqCode) {
        case 0x20:  /* SET_LINE_CODING: accept OUT data phase first */
            ep0_out_expect_line = 1;
            R8_UEP0_T_LEN = 0;
            /* OUT data phase after SETUP is DATA1 */
            R8_UEP0_CTRL  = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
            break;

        case 0x21: {  /* GET_LINE_CODING: return 7-byte struct */
            uint8_t *p = Ep0Buffer;
            p[0] = (uint8_t)(line_coding.baud_rate & 0xFF);
            p[1] = (uint8_t)((line_coding.baud_rate >> 8) & 0xFF);
            p[2] = (uint8_t)((line_coding.baud_rate >> 16) & 0xFF);
            p[3] = (uint8_t)((line_coding.baud_rate >> 24) & 0xFF);
            p[4] = line_coding.stop_bits;
            p[5] = line_coding.parity;
            p[6] = line_coding.data_bits;
            ep0_start_in(Ep0Buffer, 7, wLength);
            break;
        }

        case 0x22:  /* SET_CONTROL_LINE_STATE: save DTR/RTS, return ZLP */
            cdc_line_state = (uint8_t)(wValue & 0xFF);
            ep0_send_zlp();
            break;

        default:
            ep0_stall();
            break;
        }
    } else {
        ep0_stall();
    }
}

/*********************************************************************
 * Endpoint reset (bus reset)
 *********************************************************************/
static void usb_reset_endpoints(void)
{
    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    R8_UEP1_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
    R8_UEP2_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
    R8_UEP3_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
    R8_UEP4_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;

    R8_USB_DEV_AD = 0x00;

    usb_dev_config      = 0;
    ep1_in_ready        = 1;   /* idle: no transfer in flight */
    ep3_in_ready        = 1;   /* idle: no transfer in flight */
    addr_pending        = 0;
    ep0_out_expect_line = 0;
    ep0_in_remaining    = 0;
    ep0_in_zlp          = 0;
    cdc_rx_head         = 0;
    cdc_rx_tail         = 0;
    cdc_line_state      = 0;
    cdc_tx_head         = 0;
    cdc_tx_tail         = 0;
}

/*********************************************************************
 * USB interrupt handler
 *********************************************************************/
__attribute__((interrupt("WCH-Interrupt-fast")))
__attribute__((section(".highcode")))
void USB_IRQHandler(void)
{
    uint8_t int_st;
    uint8_t token;
    uint8_t ep;
    uint8_t len;

    if (R8_USB_INT_FG & RB_UIF_TRANSFER) {
        usb_transfer_irq_cnt++;
        int_st = R8_USB_INT_ST;
        if (int_st & RB_UIS_SETUP_ACT) {
            /* SETUP token received - defer processing to task().
             * NAK further OUT/IN so the 8-byte setup packet in Ep0Buffer
             * is not overwritten before the task reads it. */
            usb_setup_irq_cnt++;
            usb_setup_flag = 1;
            R8_UEP0_CTRL = UEP_R_RES_NAK | UEP_T_RES_NAK;
        } else {
            token = int_st & MASK_UIS_TOKEN;
            ep    = int_st & MASK_UIS_ENDP;
            len   = R8_USB_RX_LEN;
            if (ep == 1 && token == UIS_TOKEN_IN) {
                usb_ep1in_irq++;
                /* Restore NAK right here, as the WCH examples do.  With
                 * RB_UEP_AUTO_TOG the data toggle was already flipped by
                 * hardware.  If ACK stayed armed until the deferred task
                 * ran, the next IN poll from the host would re-transmit
                 * the stale buffer with a *valid* toggle -> the host reads
                 * duplicated data. */
                R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
            } else if (ep == 3 && token == UIS_TOKEN_IN) {
                usb_ep3in_irq++;
                R8_UEP3_CTRL = (R8_UEP3_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
            } else if (ep == 4 && token == UIS_TOKEN_OUT) {
                if (int_st & RB_UIS_TOG_OK) {
                    usb_ep4out_irq++;
                    /* NAK now so the next OUT packet cannot overwrite
                     * Ep4OutBuffer before the task copies it to the ring.
                     * The host simply retries with NAK: no loss. */
                    R8_UEP4_CTRL = (R8_UEP4_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_NAK;
                } else {
                    /* toggle-mismatched packet: nothing was received, keep
                     * accepting (the task ignores this event) */
                    R8_UEP4_CTRL = (R8_UEP4_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_ACK;
                }
            } else if (ep == 0 && token == UIS_TOKEN_IN && addr_pending) {
                /* SET_ADDRESS status ZLP was just ACKed: apply the address
                 * now, before the next (re-addressed) SETUP arrives - same
                 * place as the WCH examples do it. */
                R8_USB_DEV_AD = pending_addr;
                addr_pending  = 0;
            }
            /* push to ring buffer (drop if full) */
            uint8_t next = (uint8_t)(irq_head + 1);
            if (next >= IRQ_RING_SIZE) next = 0;
            if (next != irq_tail) {
                irq_ring[irq_head].token  = token;
                irq_ring[irq_head].ep     = ep;
                irq_ring[irq_head].len    = len;
                irq_ring[irq_head].tog_ok = (int_st & RB_UIS_TOG_OK) ? 1 : 0;
                irq_head = next;
            } else {
                usb_evt_dropped++;
            }
        }
        R8_USB_INT_FG = RB_UIF_TRANSFER;
    }

    if (R8_USB_INT_FG & RB_UIF_BUS_RST) {
        usb_reset_endpoints();
        usb_bus_reset_pending = 1;
        R8_USB_INT_FG = RB_UIF_BUS_RST;
    }

    if (R8_USB_INT_FG & RB_UIF_SUSPEND) {
        usb_suspended = 1;
        R8_USB_INT_FG = RB_UIF_SUSPEND;
    }
}

/*********************************************************************
 * Public API
 *********************************************************************/

void usb_composite_init(void)
{
    /* clear all state */
    usb_setup_flag      = 0;
    usb_dev_config      = 0;
    usb_suspended      = 0;
    ep1_in_ready        = 1;   /* idle: no transfer in flight */
    ep3_in_ready        = 1;   /* idle: no transfer in flight */
    addr_pending        = 0;
    ep0_out_expect_line = 0;
    ep0_in_remaining    = 0;
    ep0_in_zlp          = 0;
    cdc_rx_head         = 0;
    cdc_rx_tail         = 0;
    cdc_line_state      = 0;
    irq_head            = 0;
    irq_tail            = 0;
    line_coding.baud_rate = 115200;
    line_coding.stop_bits = 0;
    line_coding.parity    = 0;
    line_coding.data_bits = 8;

    /* disable USB first */
    R8_USB_CTRL = 0x00;

    /* endpoint modes:
     *   EP1 TX only, EP4 RX only
     *   EP2 TX only, EP3 TX only */
    R8_UEP4_1_MOD = RB_UEP1_TX_EN | RB_UEP4_RX_EN;
    R8_UEP2_3_MOD = RB_UEP2_TX_EN | RB_UEP3_TX_EN;

    /* DMA buffer addresses (EP4's buffer is hardware-fixed at UEP0_DMA+64;
     * EP4 RX lands in Ep4OutBuffer automatically) */
    R16_UEP0_DMA = (uint16_t)(uint32_t)EP0_Databuf;
    R16_UEP1_DMA = (uint16_t)(uint32_t)EP1_Databuf;
    R16_UEP2_DMA = (uint16_t)(uint32_t)EP2_Databuf;
    R16_UEP3_DMA = (uint16_t)(uint32_t)EP3_Databuf;

    /* endpoint handshake defaults */
    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    R8_UEP1_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
    R8_UEP2_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
    R8_UEP3_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
    R8_UEP4_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;

    R8_USB_DEV_AD = 0x00;
    R8_USB_CTRL = RB_UC_DEV_PU_EN | RB_UC_INT_BUSY | RB_UC_DMA_EN;
    R16_PIN_ANALOG_IE |= RB_PIN_USB_IE | RB_PIN_USB_DP_PU;
    R8_USB_INT_FG = 0xFF;
    R8_UDEV_CTRL = RB_UD_PD_DIS;   /* disable pulldowns first... */
    R8_USB_INT_EN = RB_UIE_SUSPEND | RB_UIE_BUS_RST | RB_UIE_TRANSFER;

    PFIC_EnableIRQ(USB_IRQn);
    R8_UDEV_CTRL |= RB_UD_PORT_EN; /* ...enable the physical port last (WCH order) */
}

void usb_composite_task(void)
{
    irq_evt_t evt;

    /* 1) 先按 FIFO 消化所有 deferred 端点事件，再处理新 SETUP。
     *    !!! 顺序至关重要 !!! 主机的控制传输 token 以微秒级连续到达
     *    （数据阶段 IN/OUT、状态阶段 OUT、下一个 SETUP）。若先处理 SETUP
     *    再处理 ring 里的旧事件，旧 IN/OUT 事件会把新 SETUP 刚配置的 EP0
     *    应答覆盖回 NAK/错误 toggle，导致主机状态阶段超时（error -110）。 */
    if (usb_bus_reset_pending) {
        usb_bus_reset_pending = 0;
        USB_LOG("[USB] BR t=%lu s=%lu drop=%u\r\n",
                (unsigned long)usb_transfer_irq_cnt,
                (unsigned long)usb_setup_irq_cnt,
                (unsigned)usb_evt_dropped);
    }

    while (irq_ring_pop(&evt)) {
        switch (evt.ep) {
        case 0:
            if (evt.token == UIS_TOKEN_IN) {
                /* EP0 IN complete: apply deferred address, send next chunk */
                if (addr_pending) {
                    R8_USB_DEV_AD = pending_addr;
                    addr_pending = 0;
                }
                if (ep0_in_remaining > 0 || ep0_in_zlp) {
                    /* continuation packet: flip the DATA toggle and send */
                    ep0_send_chunk(0);
                    USB_LOG("[USB] I0 rem=%u\r\n", (unsigned)ep0_in_remaining);
                } else {
                    /* data phase finished: keep the DATA1 toggle flag and arm
                     * for the status-stage OUT (which is DATA1).  Writing a
                     * plain UEP_R_RES_ACK|UEP_T_RES_NAK here would clear
                     * RB_UEP_R_TOG back to DATA0 and the status-stage OUT
                     * would then fail the toggle check -> enumeration hangs.
                     * (See WCH CH583 examples: "默认数据包是DATA1") */
                    R8_UEP0_T_LEN = 0;
                    R8_UEP0_CTRL  = RB_UEP_R_TOG | RB_UEP_T_TOG
                                    | UEP_R_RES_ACK | UEP_T_RES_NAK;
                    USB_LOG("[USB] I0 done\r\n");
                }
            } else if (evt.token == UIS_TOKEN_OUT && evt.tog_ok) {
                if (ep0_out_expect_line) {
                    /* SET_LINE_CODING data phase (DATA1) */
                    uint8_t *p = Ep0Buffer;
                    ep0_out_expect_line = 0;
                    line_coding.baud_rate = (uint32_t)p[0]
                                          | ((uint32_t)p[1] << 8)
                                          | ((uint32_t)p[2] << 16)
                                          | ((uint32_t)p[3] << 24);
                    line_coding.stop_bits = p[4];
                    line_coding.parity    = p[5];
                    line_coding.data_bits = p[6];
                    /* status ZLP (DATA1) */
                    R8_UEP0_T_LEN = 0;
                    R8_UEP0_CTRL  = RB_UEP_R_TOG | RB_UEP_T_TOG
                                    | UEP_R_RES_NAK | UEP_T_RES_ACK;
                    USB_LOG("[USB] O0 line\r\n");
                } else {
                    /* status-stage OUT of a control read: transfer done.
                     * Go idle (NAK) and wait for the next SETUP. */
                    R8_UEP0_T_LEN = 0;
                    R8_UEP0_CTRL  = RB_UEP_R_TOG | RB_UEP_T_TOG
                                    | UEP_R_RES_NAK | UEP_T_RES_NAK;
                    USB_LOG("[USB] O0 done\r\n");
                }
            }
            break;

        case 1:
            if (evt.token == UIS_TOKEN_IN) {
                /* NAK already restored in the IRQ; toggle flipped by
                 * RB_UEP_AUTO_TOG.  Only the completion flag is needed. */
                ep1_in_ready = 1;
            }
            break;

        case 3:
            if (evt.token == UIS_TOKEN_IN) {
                ep3_in_ready = 1;
            }
            break;

        case 4:
            if (evt.token == UIS_TOKEN_OUT && evt.tog_ok) {
                /* CDC data received on EP4 OUT -> RX ring.  EP4 was NAK'd
                 * by the IRQ right after reception, so Ep4OutBuffer cannot
                 * be overwritten while we copy it out. */
                uint8_t n = evt.len;
                uint16_t head = cdc_rx_head;
                for (uint8_t i = 0; i < n; i++) {
                    uint16_t next = (uint16_t)(head + 1);
                    if (next >= CDC_RX_RING_SIZE) next = 0;
                    if (next == cdc_rx_tail) break;   /* ring full: drop rest */
                    cdc_rx_ring[head] = Ep4OutBuffer[i];
                    head = next;
                }
                cdc_rx_head = head;
                /* re-arm EP4 OUT in one write: flip the expected RX toggle
                 * (EP4 has NO AUTO_TOG, like EP0, so it must be flipped by
                 * software) and go back to ACK. */
                R8_UEP4_CTRL = ((R8_UEP4_CTRL ^ RB_UEP_R_TOG) & ~MASK_UEP_R_RES)
                               | UEP_R_RES_ACK;
            }
            break;

        default:
            break;
        }
    }

    /* 2) 再处理 pending SETUP（必须在 ring 全部消化之后，见上方注释） */
    if (usb_setup_flag) {
        usb_setup_flag = 0;
        usb_process_setup();
    }

    /* 3) flush queued CDC TX data (non-blocking) */
    cdc_tx_pump();

    /* ---- Async USB HID type state machine: 每次调用最多推一步 ---- */
    if (s_ut_busy) {
        uint32_t now = TMOS_GetSystemClock();   /* ms counter (BLE lib) */
        switch (s_ut_step) {
            case USB_STEP_PRESS: {
                /* 按键码已在入队时预解析（含特殊键/转义），直接发送 */
                uint8_t mod = s_ut_seq[s_ut_idx].mod;
                uint8_t kc  = s_ut_seq[s_ut_idx].kc;
                uint8_t *p = Ep1InBuffer;
                p[0]=mod; p[1]=0; p[2]=kc; p[3]=0; p[4]=0; p[5]=0; p[6]=0; p[7]=0;
                ep1_in_ready = 0;
                R8_UEP1_T_LEN = 8;
                R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
                s_ut_step = USB_STEP_WAIT_PRESS;
                s_ut_deadline = now + USB_STEP_DELAY_MS;
                break;
            }
            case USB_STEP_WAIT_PRESS:
                /* wait for IN-complete AND hold time before releasing, so the
                 * host is guaranteed to have seen the press report */
                if (ep1_in_ready && (int32_t)(now - s_ut_deadline) >= 0) {
                    /* release all keys */
                    uint8_t *p = Ep1InBuffer;
                    p[0]=0; p[1]=0; p[2]=0; p[3]=0; p[4]=0; p[5]=0; p[6]=0; p[7]=0;
                    ep1_in_ready = 0;
                    R8_UEP1_T_LEN = 8;
                    R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
                    s_ut_typed++;
                    s_ut_step = USB_STEP_WAIT_REL;
                    s_ut_deadline = now + USB_STEP_DELAY_MS;
                    USB_LOG("[USB] up key %02X/%02X (%d/%d)\r\n",
                            s_ut_seq[s_ut_idx].mod, s_ut_seq[s_ut_idx].kc,
                            s_ut_typed, s_ut_len);
                }
                break;

            case USB_STEP_WAIT_REL:
                if (ep1_in_ready && (int32_t)(now - s_ut_deadline) >= 0) {
                    s_ut_step = USB_STEP_NEXT;
                }
                break;

            case USB_STEP_NEXT:
                s_ut_idx++;
                if (s_ut_idx >= s_ut_len) {
                    /* 全部完成 */
                    s_ut_busy = 0;
                    s_ut_step = USB_STEP_PRESS;
                    s_ut_len = s_ut_idx = 0;
                } else {
                    s_ut_step = USB_STEP_PRESS;
                }
                break;

            case USB_STEP_RELEASE:  /* 占位，未使用 */
            default:
                s_ut_step = USB_STEP_NEXT;
                break;
        }
    }
}

/* ---- Internal helper: fire EP1 report (press or release) and wait until
 *      the next usb_composite_task() cycle confirms it is out.
 *      Note: `usb_hid_send_key` is kept for compatibility and still does
 *      a *single character's* full press-release dance in one blocking
 *      call, but usb_hid_send_text() now uses the async state machine. */
/* wait for the EP1 IN completion flag; return 0 on timeout (~100ms).
 * `ep1_in_ready` is set by the deferred task when the host has polled the
 * report.  If the device is not enumerated, the host never polls and this
 * would loop forever, so bail out. */
static uint8_t usb_wait_ep1_in_ready(void)
{
    uint32_t t0 = TMOS_GetSystemClock();
    while (!ep1_in_ready) {
        usb_composite_task();
        if ((int32_t)(TMOS_GetSystemClock() - t0) > 100) {
            return 0;   /* ~100ms timeout */
        }
    }
    return 1;
}

void usb_hid_send_key(uint8_t modifier, uint8_t keycode)
{
    uint8_t *p = Ep1InBuffer;

    /* press report */
    p[0] = modifier;
    p[1] = 0;
    p[2] = keycode;
    p[3] = 0;
    p[4] = 0;
    p[5] = 0;
    p[6] = 0;
    p[7] = 0;
    ep1_in_ready = 0;
    R8_UEP1_T_LEN = 8;
    R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
    /* pump the deferred queue while waiting for IN completion */
    if (!usb_wait_ep1_in_ready()) {
        /* not enumerated / no host polling: leave EP1 NAK'd and bail out */
        R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
        return;
    }

    /* release report */
    p[0] = 0;
    p[1] = 0;
    p[2] = 0;
    p[3] = 0;
    p[4] = 0;
    p[5] = 0;
    p[6] = 0;
    p[7] = 0;
    ep1_in_ready = 0;
    R8_UEP1_T_LEN = 8;
    R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
    usb_wait_ep1_in_ready();
}

#define HID_MOD_LSHIFT 0x02

static int usb_ascii_to_hid(char ch, uint8_t *modifier, uint8_t *keycode)
{
    uint8_t mod = 0;
    uint8_t kc  = 0;

    if (ch >= 'a' && ch <= 'z') {
        kc = (uint8_t)(0x04 + (ch - 'a'));
    } else if (ch >= 'A' && ch <= 'Z') {
        mod = HID_MOD_LSHIFT;
        kc  = (uint8_t)(0x04 + (ch - 'A'));
    } else if (ch >= '1' && ch <= '9') {
        kc = (uint8_t)(0x1E + (ch - '1'));
    } else if (ch == '0') {
        kc = 0x27;
    } else {
        switch (ch) {
        case '!': mod = HID_MOD_LSHIFT; kc = 0x1E; break;
        case '@': mod = HID_MOD_LSHIFT; kc = 0x1F; break;
        case '#': mod = HID_MOD_LSHIFT; kc = 0x20; break;
        case '$': mod = HID_MOD_LSHIFT; kc = 0x21; break;
        case '%': mod = HID_MOD_LSHIFT; kc = 0x22; break;
        case '^': mod = HID_MOD_LSHIFT; kc = 0x23; break;
        case '&': mod = HID_MOD_LSHIFT; kc = 0x24; break;
        case '*': mod = HID_MOD_LSHIFT; kc = 0x25; break;
        case '(': mod = HID_MOD_LSHIFT; kc = 0x26; break;
        case ')': mod = HID_MOD_LSHIFT; kc = 0x27; break;
        case ' ':  kc = 0x2C; break;
        case '-':  kc = 0x2D; break;
        case '_':  mod = HID_MOD_LSHIFT; kc = 0x2D; break;
        case '=':  kc = 0x2E; break;
        case '+':  mod = HID_MOD_LSHIFT; kc = 0x2E; break;
        case '[':  kc = 0x2F; break;
        case '{':  mod = HID_MOD_LSHIFT; kc = 0x2F; break;
        case ']':  kc = 0x30; break;
        case '}':  mod = HID_MOD_LSHIFT; kc = 0x30; break;
        case '\\': kc = 0x31; break;
        case '|':  mod = HID_MOD_LSHIFT; kc = 0x31; break;
        case ';':  kc = 0x33; break;
        case ':':  mod = HID_MOD_LSHIFT; kc = 0x33; break;
        case '\'': kc = 0x34; break;
        case '"':  mod = HID_MOD_LSHIFT; kc = 0x34; break;
        case '`':  kc = 0x35; break;
        case '~':  mod = HID_MOD_LSHIFT; kc = 0x35; break;
        case ',':  kc = 0x36; break;
        case '<':  mod = HID_MOD_LSHIFT; kc = 0x36; break;
        case '.':  kc = 0x37; break;
        case '>':  mod = HID_MOD_LSHIFT; kc = 0x37; break;
        case '/':  kc = 0x38; break;
        case '?':  mod = HID_MOD_LSHIFT; kc = 0x38; break;
        default:
            return -1;
        }
    }
    *modifier = mod;
    *keycode  = kc;
    return 0;
}

/*********************************************************************
 * @fn      usb_hid_special_key_lookup
 *
 * @brief   在特殊键名表中查找 "name" token（大小写不敏感）。
 *          命中则回填 HID 键码并返回 0，否则返回 -1。
 */
static int usb_hid_special_key_lookup(const char *name, uint16_t len, uint8_t *keycode)
{
    uint16_t i;
    for (i = 0; i < USB_SPECIAL_KEY_NUM; i++) {
        const char *n = s_usb_special_keys[i].name;
        uint16_t j;
        for (j = 0; j < len; j++) {
            char a = name[j];
            char b = n[j];
            if (b == '\0') break;              /* 表项比 token 短 */
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b) break;
        }
        if (j == len && n[j] == '\0') {        /* 完全匹配 */
            *keycode = s_usb_special_keys[i].keycode;
            return 0;
        }
    }
    return -1;
}

/*********************************************************************
 * @fn      usb_hid_parse
 *
 * @brief   把文本解析为 HID 按键序列 (modifier, keycode)。
 *          与 BLE 端 hidkbd_parse 语法一致：
 *            - 普通字符: usb_ascii_to_hid 一对一转换；
 *            - "name":   特殊键 token，如 "enter" -> 回车、"esc" -> ESC、
 *                        "f1"~"f12"、"tab"/"space"/方向键等，键名不区分大小写；
 *            - \" 和 \\: 转义符，按单个字面量字符处理（\"enter\" 会按
 *                        普通文本 enter 发送，不会被当作特殊键）；
 *            - 其他 \x:   保持原样按普通字符发送；
 *            - 无法转换/不支持的字符: 跳过不发送。
 *
 * @param   text - 待解析的 NUL 结尾字符串
 * @param   seq  - 输出按键序列缓冲
 * @param   max  - 序列缓冲容量上限
 *
 * @return  生成的按键数（0 表示无可发送内容）。
 */
static uint16_t usb_hid_parse(const char *text, usb_kbd_seq_key_t *seq, uint16_t max)
{
    uint16_t n = 0;
    const char *p = text;

    while (*p && n < max) {
        /* 转义符: 仅 " 和 \ 有转义语义，其余 \x 保持原样 */
        if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) {
            uint8_t mod = 0, kc = 0;
            if (usb_ascii_to_hid(p[1], &mod, &kc) == 0) {
                seq[n].mod = mod;
                seq[n].kc  = kc;
                n++;
            }
            p += 2;
            continue;
        }

        /* 特殊键 token: "name" */
        if (*p == '"') {
            const char *start = p + 1;
            const char *q     = start;
            while (*q && *q != '"') q++;
            if (*q == '"' && q != start) {
                uint8_t kc;
                if (usb_hid_special_key_lookup(start, (uint16_t)(q - start), &kc) == 0) {
                    seq[n].mod = 0;
                    seq[n].kc  = kc;
                    n++;
                    p = q + 1;
                    continue;
                }
            }
            /* 不是特殊键（或未闭合）: 引号按普通字符发送 */
            {
                uint8_t mod = 0, kc = 0;
                if (usb_ascii_to_hid('"', &mod, &kc) == 0) {
                    seq[n].mod = mod;
                    seq[n].kc  = kc;
                    n++;
                }
                p++;
                continue;
            }
        }

        /* 普通字符 */
        {
            uint8_t mod = 0, kc = 0;
            if (usb_ascii_to_hid(*p, &mod, &kc) == 0) {
                seq[n].mod = mod;
                seq[n].kc  = kc;
                n++;
            }
            p++;
        }
    }
    return n;
}

/* ---- Async USB text-type scheduler ---------------------------------------
 *   不阻塞，立即返回。忙状态下重复调用直接丢弃返回 0。
 *   文本在入队时被 usb_hid_parse 预解析为按键序列，返回值为按键数。 ----- */
int usb_hid_send_text(const char *text)
{
    if (!text) return 0;
    if (s_ut_busy) return 0;

    /* 转换: 字符 -> 键盘码, 支持 "enter"/"esc" 特殊键与 \" 转义 */
    uint16_t n = usb_hid_parse(text, s_ut_seq, USB_TYPE_BUF_SIZE);
    if (n == 0) return 0;

    s_ut_len   = n;   /* 按键数（解析后） */
    s_ut_idx   = 0;
    s_ut_step  = USB_STEP_PRESS;
    s_ut_typed = 0;
    s_ut_busy  = 1;
    return (int)n;
}

int usb_hid_type_busy(void)
{
    return (int)s_ut_busy;
}

int usb_hid_type_progress(void)
{
    return s_ut_typed;
}

int usb_cdc_send(const uint8_t *buf, uint16_t len)
{
    uint16_t i;
    uint16_t head;

    if (!buf || len == 0) return 0;
    if (!usb_dev_config) return 0;             /* not enumerated: drop  */

    head = cdc_tx_head;
    for (i = 0; i < len; i++) {
        uint16_t next = (uint16_t)(head + 1);
        if (next >= CDC_TX_RING_SIZE) next = 0;
        if (next == cdc_tx_tail) return (int)i;   /* ring full: send what fit */
        cdc_tx_ring[head] = buf[i];
        head = next;
    }
    cdc_tx_head = head;
    return (int)len;
}

int usb_cdc_recv(uint8_t *buf, uint16_t maxlen)
{
    uint16_t n = 0;

    if (!buf) return 0;

    while (n < maxlen && cdc_rx_tail != cdc_rx_head) {
        buf[n++] = cdc_rx_ring[cdc_rx_tail];
        if (++cdc_rx_tail >= CDC_RX_RING_SIZE) cdc_rx_tail = 0;
    }
    return (int)n;
}

uint8_t usb_cdc_available(void)
{
    uint16_t head = cdc_rx_head;
    uint16_t tail = cdc_rx_tail;
    return (uint8_t)((uint16_t)(head - tail) & (CDC_RX_RING_SIZE - 1));
}

uint8_t usb_cdc_connected(void)
{
    /* configured and host has opened the CDC port (DTR asserted) */
    return (uint8_t)((usb_dev_config != 0) && (cdc_line_state & 0x01));
}
