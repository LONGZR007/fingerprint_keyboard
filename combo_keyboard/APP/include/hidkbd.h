/********************************** (C) COPYRIGHT *******************************
 * File Name          : hidkbd.h
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2018/12/10
 * Description        :
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

#ifndef HIDKBD_H
#define HIDKBD_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************************
 * INCLUDES
 */

/*********************************************************************
 * CONSTANTS
 */

// Task Events
#define START_DEVICE_EVT          0x0001
#define START_REPORT_EVT          0x0002
#define START_PARAM_UPDATE_EVT    0x0004
#define START_PHY_UPDATE_EVT      0x0008
#define START_TYPE_EVT            0x0010   /* Async string-type state machine */
#define START_COMBO_EVT           0x0020   /* One-shot modifier+key combo (Win+L) */
/*********************************************************************
 * MACROS
 */

/*********************************************************************
 * FUNCTIONS
 */

/*********************************************************************
 * GLOBAL VARIABLES
 */

/*
 * Task Initialization for the BLE Application
 */
extern void HidEmu_Init(void);

/*
 * Task Event Processor for the BLE Application
 */
extern uint16_t HidEmu_ProcessEvent(uint8_t task_id, uint16_t events);

/*
 * Type an ASCII string (letters / digits / common punctuation) through
 * the BLE HID keyboard.  Returns the number of characters successfully
 * scheduled (>=0).  Actual transmission happens asynchronously via
 * START_TYPE_EVT events so the caller is never blocked.  If a previous
 * type is still in flight the string is dropped and 0 is returned.
 *
 * Requires HID notifications enabled (i.e. host connected).
 */
extern int  hidkbd_type_text(const char *text);

/*
 * Query whether an async string-type job is still in flight.
 */
extern int  hidkbd_type_busy(void);

/*
 * Returns the number of characters sent so far in the current or most
 * recent hidkbd_type_text() call (useful for CLI / diagnostic output
 * once the job finishes).
 */
extern int  hidkbd_type_progress(void);

/*
 * Schedule a one-shot modifier+keycode combo (e.g. Win+L: modifier=0x08,
 * keycode=0x0F) on the BLE HID master link.  press -> 100ms -> release,
 * fully asynchronous (START_COMBO_EVT).  Returns 1 if scheduled, 0 if a
 * previous combo is still in flight.
 */
extern int  hidkbd_send_combo(uint8_t modifier, uint8_t keycode);

/* Query whether a combo press/hold/release sequence is in flight. */
extern int  hidkbd_combo_busy(void);

/*********************************************************************
*********************************************************************/

#ifdef __cplusplus
}
#endif

#endif
