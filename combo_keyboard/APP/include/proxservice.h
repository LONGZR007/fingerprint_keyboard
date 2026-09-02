/********************************** (C) COPYRIGHT *******************************
 * File Name          : proxservice.h
 * Description        : Proximity Service (自定义 GATT 服务) 对外接口
 *
 * 服务契约:
 *   Service UUID : A5F5AA00-C263-4A0C-8E8F-9C0B7A5D3E01
 *   Control      : A5F5AA01-C263-4A0C-8E8F-9C0B7A5D3E01  (Write, 1 字节)
 *                  0x01 = 激活监测 (写方连接被标记为监测连接)
 *                  0x00 = 停止监测
 *                  要求链路已加密 (Just Works bond 即可)，明文写入被拒绝
 *   Status       : A5F5AA02-C263-4A0C-8E8F-9C0B7A5D3E01  (Notify, 3 字节)
 *                  [0] = int8 RSSI (EMA 平滑后, 设备为单一数据源)
 *                  [1] = state: 0=ARMED 1=TRIGGERED 2=COOLDOWN 3=OFF
 *                  [2] = flags 保留
 *********************************************************************************
 * Copyright (c) 2026
 *******************************************************************************/

#ifndef __PROXSERVICE_H__
#define __PROXSERVICE_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Control 特征写入命令值 */
#define PROX_CONTROL_DEACTIVATE           0x00
#define PROX_CONTROL_ACTIVATE             0x01

/* Status 通知负载长度 */
#define PROX_STATUS_LEN                   3

/*********************************************************************
 * @fn      Prox_AddService
 *
 * @brief   注册 Proximity Service 到 GATT Server (在 Prox_Init 中调用)。
 *
 * @return  bStatus_t
 */
extern bStatus_t Prox_AddService(void);

/*********************************************************************
 * @fn      ProxService_NotifyStatus
 *
 * @brief   向已订阅的监测连接 (手机 App) 发送 Status 通知。
 *          未订阅或监测连接不存在时静默丢弃。
 *
 * @param   rssi  - EMA 平滑后的 RSSI (dBm)
 * @param   state - PROX_STATE_* (见 proximity.h)
 * @param   flags - 保留字段, 恒 0
 */
extern void ProxService_NotifyStatus(int8_t rssi, uint8_t state, uint8_t flags);

#ifdef __cplusplus
}
#endif

#endif /* __PROXSERVICE_H__ */
