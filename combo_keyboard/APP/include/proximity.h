/********************************** (C) COPYRIGHT *******************************
 * File Name          : proximity.h
 * Description        : 手机距离监测 + 离开自动锁屏 (Win+L) 对外接口
 *
 * 工作方式:
 *   - 设备同时保持两条 BLE 连接: 电脑 (HID 主连接) + 手机 (监测连接)
 *   - 500ms 周期读取监测连接 RSSI, EMA(α=1/8) 平滑
 *   - RSSI 持续低于 thr_enter 达 confirm_s 秒 -> 向电脑发 Win+L 锁屏
 *   - 触发后进入 cooldown_s 冷却期, 手机回到 thr_exit 以上并稳定 3s
 *     才重新武装 (双阈值迟滞 + 冷却, 防遮挡/多径误触发)
 *   - 配置存 DataFlash 0x7000 独立区 (用户区 0~6399 与 SNV 之外)
 *********************************************************************************
 * Copyright (c) 2026
 *******************************************************************************/

#ifndef __PROXIMITY_H__
#define __PROXIMITY_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 判定状态 (与 Proximity Service Status 特征 [1] 字节契约一致) */
#define PROX_STATE_ARMED                 0   /* 监测中, 可触发            */
#define PROX_STATE_TRIGGERED             1   /* 刚发出 Win+L              */
#define PROX_STATE_COOLDOWN              2   /* 锁屏冷却期                */
#define PROX_STATE_OFF                   3   /* 功能关闭                  */

/* 锁屏通道模式 (prox_cfg_t.channel) */
#define PROX_CH_AUTO                     0   /* BLE 优先, 否则 USB        */
#define PROX_CH_BLE                      1   /* 仅 BLE (未就绪回退 USB)   */
#define PROX_CH_USB                      2   /* 仅 USB                    */
#define PROX_CH_BOTH                     3   /* BLE + USB 同时发          */

/* Win+L 组合键: Left GUI (0x08) + L (0x0F) */
#define PROX_LOCK_MODIFIER               0x08

/* 配置结构 (DataFlash 持久化, 8 字节) */
typedef struct {
    uint8_t magic;        /* 0x50, 非法则用默认值                    */
    uint8_t enabled;      /* 总开关, 默认 1                          */
    int8_t  thr_enter;    /* 触发阈值 dBm, 默认 -70                  */
    int8_t  thr_exit;     /* 恢复阈值 dBm, 默认 -62 (8dB 迟滞)       */
    uint8_t confirm_s;    /* 持续确认秒数, 默认 5                    */
    uint8_t cooldown_s;   /* 触发后冷却秒数, 默认 60                 */
    uint8_t lost_lock_s;  /* 手机失联视为离开秒数, 0=不触发(默认)    */
    uint8_t channel;      /* 锁屏通道: PROX_CH_*                     */
} prox_cfg_t;

/*********************************************************************
 * @fn      Prox_Init
 *
 * @brief   模块初始化: 加载配置、注册 hiddev 监测回调、注册 GATT 服务、
 *          启动 500ms TMOS 周期任务。在 main() 中 HidEmu_Init() 之后调用。
 */
extern void Prox_Init(void);

/*********************************************************************
 * @fn      Prox_GetCfg
 *
 * @brief   返回当前配置 (可修改)。修改后调用 Prox_SaveCfg() 持久化。
 */
extern prox_cfg_t *Prox_GetCfg(void);

/*********************************************************************
 * @fn      Prox_SaveCfg
 *
 * @brief   把当前配置写入 DataFlash (擦除 0x7000 页后重写)。
 *
 * @return  1 成功, 0 失败
 */
extern uint8_t Prox_SaveCfg(void);

/*********************************************************************
 * @fn      Prox_RestoreDefaults
 *
 * @brief   恢复全部默认配置并持久化 (thr -70/-62, confirm 5s,
 *          cooldown 60s, channel auto), 同时复位状态机。
 */
extern void Prox_RestoreDefaults(void);

/*********************************************************************
 * @fn      Prox_TestTrigger
 *
 * @brief   手动触发一次锁屏 (CLI "prox test"), 用于验证 Win+L 链路。
 *          电脑主连接未就绪时静默忽略。
 */
extern void Prox_TestTrigger(void);

/*********************************************************************
 * @fn      Prox_GetRssi / Prox_GetState / Prox_GetMasterRssi
 *
 * @brief   CLI 显示用: 监测连接 EMA 后的 RSSI、当前判定状态、
 *          最近一次主连接 RSSI 读数 (-127 表示无数据)。
 */
extern int8_t  Prox_GetRssi(void);
extern uint8_t Prox_GetState(void);
extern int8_t  Prox_GetMasterRssi(void);

/*********************************************************************
 * @fn      Prox_OpenPairing
 *
 * @brief   临时开放配对窗口 60s (关闭 bond 白名单过滤), 让手机首次
 *          配对。到期自动恢复白名单。
 */
extern void Prox_OpenPairing(void);

#ifdef __cplusplus
}
#endif

#endif /* __PROXIMITY_H__ */
