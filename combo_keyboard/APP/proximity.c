/********************************** (C) COPYRIGHT *******************************
 * File Name          : proximity.c
 * Description        : 手机距离监测 + 离开自动锁屏状态机
 *
 *   500ms TMOS 周期任务:
 *     1. 监测连接在位 -> GAPRole_ReadRssiCmd 异步读 RSSI
 *     2. RSSI 回调做 EMA(α=1/8) 平滑
 *     3. 状态机: ARMED -> TRIGGERED(发 Win+L) -> COOLDOWN -> ARMED
 *        - 进入条件: ema < thr_enter 持续 confirm_s 秒 且 电脑主连接已加密
 *        - 退出条件: cooldown 计满 且 ema > thr_exit 持续 3s
 *     4. 每拍通过 Proximity Service notify 上报 RSSI + 状态给手机 App
 *
 *   配置持久化: DataFlash 0x7000 (256B 页, 独立于用户区 0~6399 与 SNV)。
 *********************************************************************************
 * Copyright (c) 2026
 *******************************************************************************/

/*********************************************************************
 * INCLUDES
 */
#include "CONFIG.h"
#include <string.h>
#include "hiddev.h"
#include "hidkbd.h"
#include "keyboard_dispatch.h"
#include "proxservice.h"
#include "proximity.h"

/*********************************************************************
 * MACROS / CONSTANTS
 */
// Task events
#define PROX_TASK_EVT_POLL               0x0001

// Poll period in ms
#define PROX_POLL_MS                     500

// Config flash layout (independent page, 256B erase granularity)
#define PROX_FLASH_ADDR                  0x7000
#define PROX_FLASH_PAGE                  256
#define PROX_CFG_MAGIC                   0x50

// Signal-good confirm time after cooldown (in poll ticks, 6*500ms = 3s)
#define PROX_EXIT_CONFIRM_TICKS          6

// HID keycode for 'L'
#define PROX_LOCK_KEYCODE                HID_KEYBOARD_L

// Poll ticks per second
#define PROX_TICKS_PER_SEC               (1000 / PROX_POLL_MS)

/*********************************************************************
 * LOCAL VARIABLES
 */
static uint8_t prox_task_id = INVALID_TASK_ID;

// Runtime configuration (loaded from DataFlash, defaults if invalid)
static prox_cfg_t s_cfg;

// Distance state machine
static uint8_t  s_state = PROX_STATE_OFF;
static int8_t   s_rssi_ema = 0;         // EMA-smoothed monitor link RSSI
static uint8_t  s_rssi_valid = 0;       // 1 once enough valid RSSI samples
static uint8_t  s_sample_cnt = 0;       // valid RSSI sample count
static int8_t   s_master_rssi = -127;   // last master link RSSI (CLI)
static uint8_t  s_low_ticks = 0;        // consecutive low-signal ticks
static uint8_t  s_good_ticks = 0;       // consecutive good-signal ticks
static uint16_t s_cooldown_left = 0;    // remaining cooldown in ticks
static uint16_t s_lost_ticks = 0;       // monitor-link-lost ticks

/*********************************************************************
 * LOCAL FUNCTIONS
 */
static void prox_cfg_defaults(void);
static uint8_t prox_cfg_load(void);
static void prox_set_state(uint8_t st);
static kbd_channel_t prox_effective_channel(void);
static void prox_fire_lock(void);
static void proxMonConnCB(uint8_t evt, uint16_t connHandle);
static void proxRssiCB(uint16_t connHandle, int8_t rssi);
static uint16_t ProxTask_ProcessEvent(uint8_t task_id, uint16_t events);

/*********************************************************************
 * @fn      prox_cfg_defaults
 */
static void prox_cfg_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.magic       = PROX_CFG_MAGIC;
    s_cfg.enabled     = 1;
    s_cfg.thr_enter   = -70;
    s_cfg.thr_exit    = -62;
    s_cfg.confirm_s   = 5;
    s_cfg.cooldown_s  = 60;
    s_cfg.lost_lock_s = 0;
    s_cfg.channel     = PROX_CH_AUTO;
}

/*********************************************************************
 * @fn      prox_cfg_load
 *
 * @brief   Load config from DataFlash; fall back to defaults when the
 *          stored block is invalid (fresh device / erased page).
 */
static uint8_t prox_cfg_load(void)
{
    uint8_t buf[sizeof(prox_cfg_t)];

    if(EEPROM_READ(PROX_FLASH_ADDR, buf, sizeof(buf)) == 0 &&
       buf[0] == PROX_CFG_MAGIC)
    {
        memcpy(&s_cfg, buf, sizeof(s_cfg));
        return 1;
    }

    prox_cfg_defaults();
    return 0;
}

/*********************************************************************
 * @fn      prox_set_state
 */
static void prox_set_state(uint8_t st)
{
    s_state = st;
}

/*********************************************************************
 * @fn      prox_effective_channel
 *
 * @brief   Map cfg.channel to the keyboard_dispatch channel mask,
 *          honouring the BLE master link readiness for AUTO/BLE.
 */
static kbd_channel_t prox_effective_channel(void)
{
    switch(s_cfg.channel)
    {
        case PROX_CH_USB:
            return KBD_CH_USB;

        case PROX_CH_BOTH:
            return KBD_CH_BOTH;

        case PROX_CH_BLE:
        /* fall through: BLE 未就绪回退 USB */
        case PROX_CH_AUTO:
        default:
            return HidDev_MasterSecure() ? KBD_CH_BLE : KBD_CH_USB;
    }
}

/*********************************************************************
 * @fn      prox_fire_lock
 *
 * @brief   Send the Win+L lock combo over the configured channel(s).
 */
static void prox_fire_lock(void)
{
    keyboard_send_combo(PROX_LOCK_MODIFIER, PROX_LOCK_KEYCODE,
                        prox_effective_channel());
}

/*********************************************************************
 * @fn      proxMonConnCB
 *
 * @brief   Monitor link event callback (registered with hiddev).
 */
static void proxMonConnCB(uint8_t evt, uint16_t connHandle)
{
    (void)connHandle;

    if(evt == HIDDEV_MON_CONN_ESTABLISHED)
    {
        // fresh link -> fresh state machine: never inherit cooldown/trigger
        // state from a previous session, and never mis-fire on stale stats
        s_rssi_valid    = 0;
        s_rssi_ema      = 0;
        s_sample_cnt    = 0;
        s_low_ticks     = 0;
        s_good_ticks    = 0;
        s_lost_ticks    = 0;
        s_cooldown_left = 0;
        if(s_cfg.enabled)
        {
            prox_set_state(PROX_STATE_ARMED);
        }
    }
    else if(evt == HIDDEV_MON_CONN_TERMINATED)
    {
        // link gone: no distance judgement until the phone is back
        s_rssi_valid = 0;
        s_low_ticks  = 0;
        s_good_ticks = 0;
        s_lost_ticks = 0;
    }
}

/*********************************************************************
 * @fn      proxRssiCB
 *
 * @brief   RSSI read callback (registered with hiddev).  The monitor
 *          link value is EMA-smoothed; the master link value is kept
 *          for the CLI "rssi" command.
 */
static void proxRssiCB(uint16_t connHandle, int8_t rssi)
{
    if(connHandle == HidDev_MonConnHandle())
    {
        // 过滤链路协商期的无效采样 (0 / 正值 / -127 通常是读失败)
        if(rssi > -10 || rssi < -100)
        {
            return;
        }

        if(s_rssi_valid)
        {
            // EMA with alpha = 1/8
            s_rssi_ema = (int8_t)(((int16_t)s_rssi_ema * 7 + rssi) >> 3);
        }
        else
        {
            s_rssi_ema = rssi;
            // 至少 2 个有效采样后才参与判定, 防首样本异常
            if(++s_sample_cnt >= 2)
            {
                s_rssi_valid = 1;
            }
        }
    }
    else if(connHandle == HidDev_MasterHandle())
    {
        s_master_rssi = rssi;
    }
}

/*********************************************************************
 * @fn      ProxTask_ProcessEvent
 *
 * @brief   500ms periodic task: RSSI sampling + state machine + notify.
 */
static uint16_t ProxTask_ProcessEvent(uint8_t task_id, uint16_t events)
{
    (void)task_id;

    if(events & PROX_TASK_EVT_POLL)
    {
        if(!s_cfg.enabled)
        {
            prox_set_state(PROX_STATE_OFF);
        }
        else if(HidDev_MonConnActive())
        {
            s_lost_ticks = 0;

            // async RSSI sample for this tick
            GAPRole_ReadRssiCmd(HidDev_MonConnHandle());

            switch(s_state)
            {
                case PROX_STATE_ARMED:
                    if(s_rssi_valid && s_rssi_ema < s_cfg.thr_enter)
                    {
                        if(s_low_ticks < 255) s_low_ticks++;
                    }
                    else
                    {
                        s_low_ticks = 0;
                    }

                    if(s_low_ticks >= (uint8_t)(s_cfg.confirm_s * PROX_TICKS_PER_SEC))
                    {
                        // 触发即按配置通道投递 (dispatch 负责通道可达性:
                        // BLE 需主连接加密, USB 未枚举时发送函数自动放弃)
                        s_low_ticks = 0;
                        prox_fire_lock();
                        prox_set_state(PROX_STATE_TRIGGERED);
                        s_cooldown_left = (uint16_t)s_cfg.cooldown_s * PROX_TICKS_PER_SEC;
                        s_good_ticks = 0;
                    }
                    break;

                case PROX_STATE_TRIGGERED:
                    // one-tick display state, then cool down
                    prox_set_state(PROX_STATE_COOLDOWN);
                    break;

                case PROX_STATE_COOLDOWN:
                    if(s_cooldown_left > 0)
                    {
                        s_cooldown_left--;
                    }
                    if(s_rssi_valid && s_rssi_ema > s_cfg.thr_exit)
                    {
                        if(s_good_ticks < 255) s_good_ticks++;
                    }
                    else
                    {
                        s_good_ticks = 0;
                    }
                    if(s_cooldown_left == 0 &&
                       s_good_ticks >= PROX_EXIT_CONFIRM_TICKS)
                    {
                        prox_set_state(PROX_STATE_ARMED);
                    }
                    break;

                case PROX_STATE_OFF:
                default:
                    prox_set_state(PROX_STATE_ARMED);
                    break;
            }
        }
        else
        {
            // monitor link gone
            s_rssi_valid = 0;
            s_low_ticks  = 0;

            if(s_cfg.lost_lock_s > 0)
            {
                s_lost_ticks++;
                if(s_state == PROX_STATE_ARMED &&
                   s_lost_ticks >= s_cfg.lost_lock_s * PROX_TICKS_PER_SEC)
                {
                    s_lost_ticks = 0;
                    prox_fire_lock();
                    prox_set_state(PROX_STATE_TRIGGERED);
                    s_cooldown_left = (uint16_t)s_cfg.cooldown_s * PROX_TICKS_PER_SEC;
                    s_good_ticks = 0;
                }
            }
            else
            {
                s_lost_ticks = 0;
            }
        }

        // report RSSI + state to the phone app (dropped if not subscribed)
        ProxService_NotifyStatus(s_rssi_valid ? s_rssi_ema : 0, s_state, 0);

        // re-arm the periodic tick
        tmos_start_task(prox_task_id, PROX_TASK_EVT_POLL, PROX_POLL_MS);
        return (events ^ PROX_TASK_EVT_POLL);
    }

    return 0;
}

/*********************************************************************
 * PUBLIC FUNCTIONS
 */

void Prox_Init(void)
{
    prox_cfg_load();

    prox_task_id = TMOS_ProcessEventRegister(ProxTask_ProcessEvent);

    // hook monitor link events + RSSI reads into hiddev
    HidDev_RegisterMonitorCB(proxMonConnCB, proxRssiCB);

    // register the Proximity GATT service
    {
        bStatus_t st = Prox_AddService();
        PRINT("Prox service registered: %d\n", st);
    }

    s_state = s_cfg.enabled ? PROX_STATE_ARMED : PROX_STATE_OFF;

    tmos_start_task(prox_task_id, PROX_TASK_EVT_POLL, PROX_POLL_MS);
}

prox_cfg_t *Prox_GetCfg(void)
{
    return &s_cfg;
}

void Prox_RestoreDefaults(void)
{
    prox_cfg_defaults();
    Prox_SaveCfg();

    // 状态机同步复位
    s_state          = s_cfg.enabled ? PROX_STATE_ARMED : PROX_STATE_OFF;
    s_cooldown_left  = 0;
    s_low_ticks      = 0;
    s_good_ticks     = 0;
    s_lost_ticks     = 0;
}

uint8_t Prox_SaveCfg(void)
{
    uint8_t buf[sizeof(prox_cfg_t)];

    memcpy(buf, &s_cfg, sizeof(s_cfg));

    if(EEPROM_ERASE(PROX_FLASH_ADDR, PROX_FLASH_PAGE))
    {
        return 0;
    }

    return (EEPROM_WRITE(PROX_FLASH_ADDR, buf, sizeof(buf)) == 0);
}

void Prox_TestTrigger(void)
{
    if(!HidDev_MasterSecure())
    {
        return;
    }

    prox_fire_lock();
    prox_set_state(PROX_STATE_TRIGGERED);
    s_cooldown_left = (uint16_t)s_cfg.cooldown_s * PROX_TICKS_PER_SEC;
    s_good_ticks = 0;
}

int8_t Prox_GetRssi(void)
{
    return s_rssi_valid ? s_rssi_ema : 0;
}

uint8_t Prox_GetState(void)
{
    return s_state;
}

int8_t Prox_GetMasterRssi(void)
{
    return s_master_rssi;
}

void Prox_OpenPairing(void)
{
    HidDev_OpenPairingWindow(60);
}
