/********************************** (C) COPYRIGHT *******************************
 * File Name          : hidkbd.c
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2018/12/10
 * Description        : 蓝牙键盘应用程序，初始化广播连接参数，然后广播，直至连接主机后，定时上传键值
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/*********************************************************************
 * INCLUDES
 */
#include "CONFIG.h"
#include "devinfoservice.h"
#include "battservice.h"
#include "hidkbdservice.h"
#include "hiddev.h"
#include "hidkbd.h"
#include <string.h>

/*********************************************************************
 * MACROS
 */
// HID keyboard input report length
#define HID_KEYBOARD_IN_RPT_LEN              8

// HID LED output report length
#define HID_LED_OUT_RPT_LEN                  1

/* Convenience wrapper for the old single-keycode callers: sends a single
 * keycode with no modifier bits.  Kept as a macro so existing call sites in
 * HidEmu_ProcessEvent can stay 1-line and still use the shared 2-arg impl. */
#define HIDEMU_SEND_KBD_SINGLE(c)   hidEmuSendKbdReport(0, (c))

/*********************************************************************
 * CONSTANTS
 */
// Param update delay
#define START_PARAM_UPDATE_EVT_DELAY         12800

// Param update delay
#define START_PHY_UPDATE_DELAY               1600

// HID idle timeout in msec; set to zero to disable timeout
#define DEFAULT_HID_IDLE_TIMEOUT             60000

// Minimum connection interval (units of 1.25ms)
#define DEFAULT_DESIRED_MIN_CONN_INTERVAL    8

// Maximum connection interval (units of 1.25ms)
#define DEFAULT_DESIRED_MAX_CONN_INTERVAL    8

// Slave latency to use if parameter update request
#define DEFAULT_DESIRED_SLAVE_LATENCY        0

// Supervision timeout value (units of 10ms)
#define DEFAULT_DESIRED_CONN_TIMEOUT         500

// Default passcode
#define DEFAULT_PASSCODE                     0

// Default GAP pairing mode
#define DEFAULT_PAIRING_MODE                 GAPBOND_PAIRING_MODE_WAIT_FOR_REQ

// Default MITM mode (TRUE to require passcode or OOB when pairing)
#define DEFAULT_MITM_MODE                    FALSE

// Default bonding mode, TRUE to bond
#define DEFAULT_BONDING_MODE                 TRUE

// Default GAP bonding I/O capabilities
#define DEFAULT_IO_CAPABILITIES              GAPBOND_IO_CAP_NO_INPUT_NO_OUTPUT

// Battery level is critical when it is less than this %
#define DEFAULT_BATT_CRITICAL_LEVEL          6

/*********************************************************************
 * TYPEDEFS
 */

/*********************************************************************
 * GLOBAL VARIABLES
 */

// Task ID
static uint8_t hidEmuTaskId = INVALID_TASK_ID;

/*********************************************************************
 * EXTERNAL VARIABLES
 */

/*********************************************************************
 * EXTERNAL FUNCTIONS
 */

/*********************************************************************
 * LOCAL VARIABLES
 */

// GAP Profile - Name attribute for SCAN RSP data
static uint8_t scanRspData[] = {
    0x05, // length of this data
    GAP_ADTYPE_SLAVE_CONN_INTERVAL_RANGE,
    LO_UINT16(DEFAULT_DESIRED_MIN_CONN_INTERVAL), // 100ms
    HI_UINT16(DEFAULT_DESIRED_MIN_CONN_INTERVAL),
    LO_UINT16(DEFAULT_DESIRED_MAX_CONN_INTERVAL), // 1s
    HI_UINT16(DEFAULT_DESIRED_MAX_CONN_INTERVAL),

    // service UUIDs
    0x05, // length of this data
    GAP_ADTYPE_16BIT_MORE,
    LO_UINT16(HID_SERV_UUID),
    HI_UINT16(HID_SERV_UUID),
    LO_UINT16(BATT_SERV_UUID),
    HI_UINT16(BATT_SERV_UUID),

    // Tx power level
    0x02, // length of this data
    GAP_ADTYPE_POWER_LEVEL,
    0 // 0dBm
};

// Advertising data
static uint8_t advertData[] = {
    // flags
    0x02, // length of this data
    GAP_ADTYPE_FLAGS,
    GAP_ADTYPE_FLAGS_LIMITED | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED,

    // appearance
    0x03, // length of this data
    GAP_ADTYPE_APPEARANCE,
    LO_UINT16(GAP_APPEARE_HID_KEYBOARD),
    HI_UINT16(GAP_APPEARE_HID_KEYBOARD),

    0x0D,                           // length of this data
    GAP_ADTYPE_LOCAL_NAME_COMPLETE, // AD Type = Complete local name
    'H',
    'I',
    'D',
    ' ',
    'K',
    'e',
    'y',
    'b',
    'o',
    'a',
    'r',
    'd',  // connection interval range
};

// Device name attribute value
static const uint8_t attDeviceName[GAP_DEVICE_NAME_LEN] = "HID Keyboard";

// HID Dev configuration
static hidDevCfg_t hidEmuCfg = {
    DEFAULT_HID_IDLE_TIMEOUT, // Idle timeout
    HID_FEATURE_FLAGS         // HID feature flags
};

static uint16_t hidEmuConnHandle = GAP_CONNHANDLE_INIT;

/* --------------------------------------------------------------------
 * Async string-type state machine.
 *
 * 每 tick = 625us。按键节奏：
 *   press -> 2 ticks (~1.25ms) -> release -> 2 ticks (~1.25ms) -> next press
 * 每个字符跨 3 个 START_TYPE_EVT 事件推进：
 *   STEP_PRESS  -> 发送按下报告，2 ticks 后进入 STEP_RELEASE
 *   STEP_RELEASE-> 发送全零释放报告，2 ticks 后进入 STEP_NEXT
 *   STEP_NEXT   -> 索引前进，进入下一个字符 STEP_PRESS
 * 状态机只在 START_TYPE_EVT 事件中推进，绝不在事件回调里阻塞，确保
 * BLE GAP/GATT/连接事件有足够调度窗口，避免"只发前两字符"丢包。
 *
 * 复用 START_REPORT_EVT 期间的按键扫描互斥：sendStrBusy=1 时暂停原有
 * START_REPORT_EVT 自动按键测试输出，防止键盘报告通道乱序。
 * ----------------------------------------------------------------- */
#define TYPE_BUF_SIZE        128
#define TYPE_STEP_PRESS      0
#define TYPE_STEP_RELEASE    1
#define TYPE_STEP_NEXT       2
#define TYPE_TICK_STEP       50   /* 每 step 之间的 tick 间隔 (~1.25ms) */

/* ---- Combo (Win+L style) one-shot sequence ---------------------------- */
#define COMBO_HOLD_TICKS     160   /* press->release hold: 160 ticks = 100ms */
static uint8_t s_combo_busy = 0;
static uint8_t s_combo_mod = 0;
static uint8_t s_combo_kc  = 0;

/* 预解析的按键序列项: (HID modifier, HID keycode) */
typedef struct {
    uint8_t mod;
    uint8_t kc;
} kbd_seq_key_t;

/* 特殊键名表: "enter" -> 回车, "esc" -> ESC ... 键名大小写不敏感 */
typedef struct {
    const char *name;
    uint8_t     keycode;
} hid_special_key_t;

static const hid_special_key_t s_special_keys[] = {
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

#define SPECIAL_KEY_NUM (sizeof(s_special_keys) / sizeof(s_special_keys[0]))

static kbd_seq_key_t s_key_seq[TYPE_BUF_SIZE];  /* 入队时预解析的按键序列 */
static uint16_t      s_key_len = 0;
static uint16_t      s_key_idx = 0;
static uint8_t       s_type_step   = TYPE_STEP_PRESS;
static volatile uint8_t s_type_busy = 0;
static int           s_type_typed = 0;

/*********************************************************************
 * LOCAL FUNCTIONS
 */

static void    hidEmu_ProcessTMOSMsg(tmos_event_hdr_t *pMsg);
static void    hidEmuSendKbdReport(uint8_t modifier, uint8_t keycode);
static uint8_t hidEmuRcvReport(uint8_t len, uint8_t *pData);
static uint8_t hidEmuRptCB(uint8_t id, uint8_t type, uint16_t uuid,
                           uint8_t oper, uint16_t *pLen, uint8_t *pData);
static void    hidEmuEvtCB(uint8_t evt);
static void    hidEmuStateCB(gapRole_States_t newState, gapRoleEvent_t *pEvent);
static int     ascii_to_hid(char ch, uint8_t *modifier, uint8_t *keycode);

/*********************************************************************
 * PROFILE CALLBACKS
 */

static hidDevCB_t hidEmuHidCBs = {
    hidEmuRptCB,
    hidEmuEvtCB,
    NULL,
    hidEmuStateCB};

/*********************************************************************
 * PUBLIC FUNCTIONS
 */

/*********************************************************************
 * @fn      HidEmu_Init
 *
 * @brief   Initialization function for the HidEmuKbd App Task.
 *          This is called during initialization and should contain
 *          any application specific initialization (ie. hardware
 *          initialization/setup, table initialization, power up
 *          notificaiton ... ).
 *
 * @param   task_id - the ID assigned by TMOS.  This ID should be
 *                    used to send messages and set timers.
 *
 * @return  none
 */
void HidEmu_Init()
{
    hidEmuTaskId = TMOS_ProcessEventRegister(HidEmu_ProcessEvent);

    // Setup the GAP Peripheral Role Profile
    {
        uint8_t initial_advertising_enable = TRUE;

        // Set the GAP Role Parameters
        GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &initial_advertising_enable);

        GAPRole_SetParameter(GAPROLE_ADVERT_DATA, sizeof(advertData), advertData);
        GAPRole_SetParameter(GAPROLE_SCAN_RSP_DATA, sizeof(scanRspData), scanRspData);
    }

    // Set the GAP Characteristics
    GGS_SetParameter(GGS_DEVICE_NAME_ATT, sizeof(attDeviceName), (void *)attDeviceName);

    // Setup the GAP Bond Manager
    {
        uint32_t passkey = DEFAULT_PASSCODE;
        uint8_t  pairMode = DEFAULT_PAIRING_MODE;
        uint8_t  mitm = DEFAULT_MITM_MODE;
        uint8_t  ioCap = DEFAULT_IO_CAPABILITIES;
        uint8_t  bonding = DEFAULT_BONDING_MODE;
        GAPBondMgr_SetParameter(GAPBOND_PERI_DEFAULT_PASSCODE, sizeof(uint32_t), &passkey);
        GAPBondMgr_SetParameter(GAPBOND_PERI_PAIRING_MODE, sizeof(uint8_t), &pairMode);
        GAPBondMgr_SetParameter(GAPBOND_PERI_MITM_PROTECTION, sizeof(uint8_t), &mitm);
        GAPBondMgr_SetParameter(GAPBOND_PERI_IO_CAPABILITIES, sizeof(uint8_t), &ioCap);
        GAPBondMgr_SetParameter(GAPBOND_PERI_BONDING_ENABLED, sizeof(uint8_t), &bonding);
    }

    // Setup Battery Characteristic Values
    {
        uint8_t critical = DEFAULT_BATT_CRITICAL_LEVEL;
        Batt_SetParameter(BATT_PARAM_CRITICAL_LEVEL, sizeof(uint8_t), &critical);
    }

    // Set up HID keyboard service
    Hid_AddService();

    // Register for HID Dev callback
    HidDev_Register(&hidEmuCfg, &hidEmuHidCBs);

    // Setup a delayed profile startup
    tmos_set_event(hidEmuTaskId, START_DEVICE_EVT);
}

/*********************************************************************
 * @fn      HidEmu_ProcessEvent
 *
 * @brief   HidEmuKbd Application Task event processor.  This function
 *          is called to process all events for the task.  Events
 *          include timers, messages and any other user defined events.
 *
 * @param   task_id  - The TMOS assigned task ID.
 * @param   events - events to process.  This is a bit map and can
 *                   contain more than one event.
 *
 * @return  events not processed
 */
uint16_t HidEmu_ProcessEvent(uint8_t task_id, uint16_t events)
{
    static uint8_t send_char = 4;

    if(events & SYS_EVENT_MSG)
    {
        uint8_t *pMsg;

        if((pMsg = tmos_msg_receive(hidEmuTaskId)) != NULL)
        {
            hidEmu_ProcessTMOSMsg((tmos_event_hdr_t *)pMsg);

            // Release the TMOS message
            tmos_msg_deallocate(pMsg);
        }

        // return unprocessed events
        return (events ^ SYS_EVENT_MSG);
    }

    if(events & START_DEVICE_EVT)
    {
        return (events ^ START_DEVICE_EVT);
    }

    if(events & START_PARAM_UPDATE_EVT)
    {
        // Send connect param update request
        GAPRole_PeripheralConnParamUpdateReq(hidEmuConnHandle,
                                             DEFAULT_DESIRED_MIN_CONN_INTERVAL,
                                             DEFAULT_DESIRED_MAX_CONN_INTERVAL,
                                             DEFAULT_DESIRED_SLAVE_LATENCY,
                                             DEFAULT_DESIRED_CONN_TIMEOUT,
                                             hidEmuTaskId);

        return (events ^ START_PARAM_UPDATE_EVT);
    }

    if(events & START_PHY_UPDATE_EVT)
    {
        // start phy update
        PRINT("Send Phy Update %x...\n", GAPRole_UpdatePHY(hidEmuConnHandle, 0, 
                    GAP_PHY_BIT_LE_2M, GAP_PHY_BIT_LE_2M, GAP_PHY_OPTIONS_NOPRE));

        return (events ^ START_PHY_UPDATE_EVT);
    }

    if(events & START_REPORT_EVT)
    {
        if (!s_type_busy) {
            static uint8_t send_char = 4;
            HIDEMU_SEND_KBD_SINGLE(send_char);
            send_char++;
            if(send_char >= 29)
                send_char = 4;
            HIDEMU_SEND_KBD_SINGLE(0x00);
        }
        tmos_start_task(hidEmuTaskId, START_REPORT_EVT, 2000);
        return (events ^ START_REPORT_EVT);
    }

    /* ---- Async HID key-sequence type state machine ----
     *
     * 文本在入队时已解析为按键序列 s_key_seq（见 hidkbd_parse），
     * 每个按键推进 3 个 event（共 ~3.75ms）：
     *   PRESS (EVT)   → send press, 2 tick → RELEASE
     *   RELEASE (EVT) → send 0x00 release, 2 tick → NEXT
     *   NEXT (EVT)    → idx++, 0 tick → PRESS of next key
     *
     * 绝不在此 busy-wait；所有推进由 tmos 定时事件驱动，
     * 保证 BLE 协议栈连接事件/通知队列有充分调度窗口。 */
    if(events & START_TYPE_EVT)
    {
        if (!s_type_busy) {
            return (events ^ START_TYPE_EVT);
        }
        switch (s_type_step) {
            case TYPE_STEP_PRESS: {
                /* 按键码已在入队时预解析（含特殊键/转义），直接发送 */
                uint8_t mod = s_key_seq[s_key_idx].mod;
                uint8_t kc  = s_key_seq[s_key_idx].kc;
                hidEmuSendKbdReport(mod, kc);
                s_type_step = TYPE_STEP_RELEASE;
                tmos_start_task(hidEmuTaskId, START_TYPE_EVT, TYPE_TICK_STEP);
                break;
            }
            case TYPE_STEP_RELEASE:
                hidEmuSendKbdReport(0, 0);
                s_type_typed++;
                s_type_step = TYPE_STEP_NEXT;
                tmos_start_task(hidEmuTaskId, START_TYPE_EVT, TYPE_TICK_STEP);
                break;

            case TYPE_STEP_NEXT:
                s_key_idx++;
                if (s_key_idx >= s_key_len) {
                    /* 全部发送完成 */
                    s_type_busy = 0;
                    s_type_step = TYPE_STEP_PRESS;
                    s_key_len = s_key_idx = 0;
                } else {
                    s_type_step = TYPE_STEP_PRESS;
                    tmos_start_task(hidEmuTaskId, START_TYPE_EVT, 0);
                }
                break;
        }
        return (events ^ START_TYPE_EVT);
    }

    /* ---- One-shot combo sequence: press -> hold -> release ---- */
    if(events & START_COMBO_EVT)
    {
        if (!s_combo_busy) {
            return (events ^ START_COMBO_EVT);
        }
        if (s_combo_busy == 1) {
            /* press */
            hidEmuSendKbdReport(s_combo_mod, s_combo_kc);
            s_combo_busy = 2;
            tmos_start_task(hidEmuTaskId, START_COMBO_EVT, COMBO_HOLD_TICKS);
        } else {
            /* release */
            hidEmuSendKbdReport(0, 0);
            s_combo_busy = 0;
        }
        return (events ^ START_COMBO_EVT);
    }
    return 0;
}

/*********************************************************************
 * @fn      hidEmu_ProcessTMOSMsg
 *
 * @brief   Process an incoming task message.
 *
 * @param   pMsg - message to process
 *
 * @return  none
 */
static void hidEmu_ProcessTMOSMsg(tmos_event_hdr_t *pMsg)
{
    switch(pMsg->event)
    {
        default:
            break;
    }
}

/*********************************************************************
 * @fn      hidEmuSendKbdReport
 *
 * @brief   Build and send a HID keyboard report.
 *
 * @param   modifier - HID modifier bitmap (bit0=LCTRL, bit1=LSHIFT, ...).
 * @param   keycode  - HID keycode.
 *
 * @return  none
 */
static void hidEmuSendKbdReport(uint8_t modifier, uint8_t keycode)
{
    uint8_t buf[HID_KEYBOARD_IN_RPT_LEN];

    buf[0] = modifier; // Modifier keys
    buf[1] = 0;        // Reserved
    buf[2] = keycode;  // Keycode 1
    buf[3] = 0;        // Keycode 2
    buf[4] = 0;        // Keycode 3
    buf[5] = 0;        // Keycode 4
    buf[6] = 0;        // Keycode 5
    buf[7] = 0;        // Keycode 6

    HidDev_Report(HID_RPT_ID_KEY_IN, HID_REPORT_TYPE_INPUT,
                  HID_KEYBOARD_IN_RPT_LEN, buf);
}

/* -------------------------------------------------------------------
 * HID modifier bitmap and keycode mapping for printable ASCII.
 *
 * Standard USB HID keycodes:
 *   a-z        -> 0x04..0x1D   (shift for uppercase)
 *   '1'-'9'    -> 0x1E..0x26
 *   '0'        -> 0x27
 *   '!'..')'   -> 1-0 with shift
 *   ' '        -> 0x2C
 *   '-' / '_'  -> 0x2D / 0x2D+shift
 *   '=' / '+'  -> 0x2E / 0x2E+shift
 *   '[' / '{'  -> 0x2F / 0x2F+shift
 *   ']' / '}'  -> 0x30 / 0x30+shift
 *   '\\' / '|' -> 0x31 / 0x31+shift
 *   ';' / ':'  -> 0x33 / 0x33+shift
 *   '\'' / '"' -> 0x34 / 0x34+shift
 *   '`' / '~'  -> 0x35 / 0x35+shift
 *   ',' / '<'  -> 0x36 / 0x36+shift
 *   '.' / '>'  -> 0x37 / 0x37+shift
 *   '/' / '?'  -> 0x38 / 0x38+shift
 * ----------------------------------------------------------------- */

#define HID_MOD_LSHIFT     0x02

/*
 * Convert one ASCII character to (modifier, keycode) pair.
 * Returns 0 on success, -1 for unsupported characters.
 */
static int ascii_to_hid(char ch, uint8_t *modifier, uint8_t *keycode)
{
    uint8_t mod = 0;
    uint8_t kc  = 0;

    if (ch >= 'a' && ch <= 'z') {
        kc = 0x04 + (uint8_t)(ch - 'a');
    } else if (ch >= 'A' && ch <= 'Z') {
        mod = HID_MOD_LSHIFT;
        kc  = 0x04 + (uint8_t)(ch - 'A');
    } else if (ch >= '1' && ch <= '9') {
        kc = 0x1E + (uint8_t)(ch - '1');
    } else if (ch == '0') {
        kc = 0x27;
    } else {
        switch (ch) {
            /* digits + shift => shifted-number symbols */
            case '!': mod = HID_MOD_LSHIFT; kc = 0x1E; break; /* 1 */
            case '@': mod = HID_MOD_LSHIFT; kc = 0x1F; break; /* 2 */
            case '#': mod = HID_MOD_LSHIFT; kc = 0x20; break; /* 3 */
            case '$': mod = HID_MOD_LSHIFT; kc = 0x21; break; /* 4 */
            case '%': mod = HID_MOD_LSHIFT; kc = 0x22; break; /* 5 */
            case '^': mod = HID_MOD_LSHIFT; kc = 0x23; break; /* 6 */
            case '&': mod = HID_MOD_LSHIFT; kc = 0x24; break; /* 7 */
            case '*': mod = HID_MOD_LSHIFT; kc = 0x25; break; /* 8 */
            case '(': mod = HID_MOD_LSHIFT; kc = 0x26; break; /* 9 */
            case ')': mod = HID_MOD_LSHIFT; kc = 0x27; break; /* 0 */
            case ' ': kc = 0x2C; break;
            case '-': kc = 0x2D; break;
            case '_': mod = HID_MOD_LSHIFT; kc = 0x2D; break;
            case '=': kc = 0x2E; break;
            case '+': mod = HID_MOD_LSHIFT; kc = 0x2E; break;
            case '[': kc = 0x2F; break;
            case '{': mod = HID_MOD_LSHIFT; kc = 0x2F; break;
            case ']': kc = 0x30; break;
            case '}': mod = HID_MOD_LSHIFT; kc = 0x30; break;
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
 * @fn      hid_special_key_lookup
 *
 * @brief   在特殊键名表中查找 "name" token（大小写不敏感）。
 *          命中则回填 HID 键码并返回 0，否则返回 -1。
 */
static int hid_special_key_lookup(const char *name, uint16_t len, uint8_t *keycode)
{
    uint16_t i;
    for (i = 0; i < SPECIAL_KEY_NUM; i++) {
        const char *n = s_special_keys[i].name;
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
            *keycode = s_special_keys[i].keycode;
            return 0;
        }
    }
    return -1;
}

/*********************************************************************
 * @fn      hidkbd_parse
 *
 * @brief   把文本解析为 HID 按键序列 (modifier, keycode)。
 *
 *          语法规则：
 *            - 普通字符: ascii_to_hid 一对一转换；
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
static uint16_t hidkbd_parse(const char *text, kbd_seq_key_t *seq, uint16_t max)
{
    uint16_t n = 0;
    const char *p = text;

    while (*p && n < max) {
        /* 转义符: 仅 " 和 \ 有转义语义，其余 \x 保持原样 */
        if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) {
            uint8_t mod = 0, kc = 0;
            if (ascii_to_hid(p[1], &mod, &kc) == 0) {
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
                if (hid_special_key_lookup(start, (uint16_t)(q - start), &kc) == 0) {
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
                if (ascii_to_hid('"', &mod, &kc) == 0) {
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
            if (ascii_to_hid(*p, &mod, &kc) == 0) {
                seq[n].mod = mod;
                seq[n].kc  = kc;
                n++;
            }
            p++;
        }
    }
    return n;
}

/*********************************************************************
 * @fn      hidkbd_type_text
 *
 * @brief   Schedule a NUL-terminated ASCII string to be typed through
 *          the BLE HID keyboard asynchronously via START_TYPE_EVT.
 *          文本在入队时被 hidkbd_parse 预解析为 HID 按键序列
 *          （普通字符一对一、"enter"/"esc" 等特殊键、\" 转义）。
 *          The caller is NOT blocked. A busy-flag is returned to
 *          callers: if a previous type is still in flight the new
 *          string is DROPPED and 0 is returned.  Actual key count
 *          sent is reported via hidkbd_type_progress() once
 *          hidkbd_type_busy() returns 0.
 *
 * @return  number of keys scheduled (>=0).  If ==0 either the
 *          input was empty/NULL or a previous job is still running.
 */
int hidkbd_type_text(const char *text)
{
    if (!text) return 0;
    if (s_type_busy) return 0;   /* in-flight, cannot accept */

    /* 转换: 字符 -> 键盘码, 支持 "enter"/"esc" 特殊键与 \" 转义 */
    uint16_t n = hidkbd_parse(text, s_key_seq, TYPE_BUF_SIZE);
    if (n == 0) return 0;

    /* Arm state machine from the start */
    s_key_len   = n;
    s_key_idx   = 0;
    s_type_step = TYPE_STEP_PRESS;
    s_type_typed = 0;
    s_type_busy  = 1;

    /* Fire first event immediately (TMOS). */
    tmos_set_event(hidEmuTaskId, START_TYPE_EVT);

    return (int)n;
}

/* ---- Public status accessors ---------------------------------------- */

int hidkbd_type_busy(void)
{
    return (int)s_type_busy;
}

int hidkbd_type_progress(void)
{
    return s_type_typed;
}

/**********************************************************************
 * @fn      hidkbd_send_combo
 *
 * @brief   Schedule a one-shot modifier+keycode combo (e.g. Win+L) on
 *          the BLE HID master link: press -> 100ms hold -> release,
 *          driven by START_COMBO_EVT so the caller never blocks.
 *
 * @return  1 if scheduled, 0 if a previous combo is still in flight.
 *********************************************************************/
int hidkbd_send_combo(uint8_t modifier, uint8_t keycode)
{
    if (s_combo_busy) return 0;

    s_combo_mod  = modifier;
    s_combo_kc   = keycode;
    s_combo_busy = 1;
    tmos_set_event(hidEmuTaskId, START_COMBO_EVT);
    return 1;
}

int hidkbd_combo_busy(void)
{
    return (int)s_combo_busy;
}

/*********************************************************************
 * @fn      hidEmuStateCB
 *
 * @brief   GAP state change callback.
 *
 * @param   newState - new state
 *
 * @return  none
 */
static void hidEmuStateCB(gapRole_States_t newState, gapRoleEvent_t *pEvent)
{
    switch(newState & GAPROLE_STATE_ADV_MASK)
    {
        case GAPROLE_STARTED:
        {
            uint8_t ownAddr[6];
            GAPRole_GetParameter(GAPROLE_BD_ADDR, ownAddr);
            GAP_ConfigDeviceAddr(ADDRTYPE_STATIC, ownAddr);
            PRINT("Initialized..\n");
        }
        break;

        case GAPROLE_ADVERTISING:
            if(pEvent->gap.opcode == GAP_MAKE_DISCOVERABLE_DONE_EVENT)
            {
                PRINT("Advertising..\n");
            }
            break;

        case GAPROLE_CONNECTED:
            if(pEvent->gap.opcode == GAP_LINK_ESTABLISHED_EVENT)
            {
                gapEstLinkReqEvent_t *event = (gapEstLinkReqEvent_t *)pEvent;

                // Only react to the HID master (PC) link; the phone monitor
                // link must not steal the param/PHY update target.
                if(event->connectionHandle == HidDev_MasterHandle())
                {
                    // get connection handle
                    hidEmuConnHandle = event->connectionHandle;
                    tmos_start_task(hidEmuTaskId, START_PARAM_UPDATE_EVT, START_PARAM_UPDATE_EVT_DELAY);
                    PRINT("Connected..\n");
                }
            }
            break;

        case GAPROLE_CONNECTED_ADV:
            if(pEvent->gap.opcode == GAP_MAKE_DISCOVERABLE_DONE_EVENT)
            {
                PRINT("Connected Advertising..\n");
            }
            break;

        case GAPROLE_WAITING:
            if(pEvent->gap.opcode == GAP_END_DISCOVERABLE_DONE_EVENT)
            {
                PRINT("Waiting for advertising..\n");
            }
            else if(pEvent->gap.opcode == GAP_LINK_TERMINATED_EVENT)
            {
                PRINT("Disconnected.. Reason:%x\n", pEvent->linkTerminate.reason);
            }
            else if(pEvent->gap.opcode == GAP_LINK_ESTABLISHED_EVENT)
            {
                PRINT("Advertising timeout..\n");
            }
            // Enable advertising
            {
                uint8_t initial_advertising_enable = TRUE;
                // Set the GAP Role Parameters
                GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &initial_advertising_enable);
            }
            break;

        case GAPROLE_ERROR:
            PRINT("Error %x ..\n", pEvent->gap.opcode);
            break;

        default:
            break;
    }
}

/*********************************************************************
 * @fn      hidEmuRcvReport
 *
 * @brief   Process an incoming HID keyboard report.
 *
 * @param   len - Length of report.
 * @param   pData - Report data.
 *
 * @return  status
 */
static uint8_t hidEmuRcvReport(uint8_t len, uint8_t *pData)
{
    // verify data length
    if(len == HID_LED_OUT_RPT_LEN)
    {
        // set LEDs
        return SUCCESS;
    }
    else
    {
        return ATT_ERR_INVALID_VALUE_SIZE;
    }
}

/*********************************************************************
 * @fn      hidEmuRptCB
 *
 * @brief   HID Dev report callback.
 *
 * @param   id - HID report ID.
 * @param   type - HID report type.
 * @param   uuid - attribute uuid.
 * @param   oper - operation:  read, write, etc.
 * @param   len - Length of report.
 * @param   pData - Report data.
 *
 * @return  GATT status code.
 */
static uint8_t hidEmuRptCB(uint8_t id, uint8_t type, uint16_t uuid,
                           uint8_t oper, uint16_t *pLen, uint8_t *pData)
{
    uint8_t status = SUCCESS;

    // write
    if(oper == HID_DEV_OPER_WRITE)
    {
        if(uuid == REPORT_UUID)
        {
            // process write to LED output report; ignore others
            if(type == HID_REPORT_TYPE_OUTPUT)
            {
                status = hidEmuRcvReport(*pLen, pData);
            }
        }

        if(status == SUCCESS)
        {
            status = Hid_SetParameter(id, type, uuid, *pLen, pData);
        }
    }
    // read
    else if(oper == HID_DEV_OPER_READ)
    {
        status = Hid_GetParameter(id, type, uuid, pLen, pData);
    }
    // notifications enabled
    // else if(oper == HID_DEV_OPER_ENABLE)
    // {
    //     tmos_start_task(hidEmuTaskId, START_REPORT_EVT, 500);
    // }
    return status;
}

/*********************************************************************
 * @fn      hidEmuEvtCB
 *
 * @brief   HID Dev event callback.
 *
 * @param   evt - event ID.
 *
 * @return  HID response code.
 */
static void hidEmuEvtCB(uint8_t evt)
{
    // process enter/exit suspend or enter/exit boot mode
    return;
}

/*********************************************************************
*********************************************************************/
