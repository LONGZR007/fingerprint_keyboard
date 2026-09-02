/********************************** (C) COPYRIGHT *******************************
 * File Name          : proxservice.c
 * Description        : Proximity Service - 自定义 GATT 服务
 *                      手机 App 连接后写 Control 特征 0x01 激活距离监测，
 *                      该连接被标记为"监测连接" (HidDev_MarkMonitorConnection)；
 *                      设备通过 Status 特征周期 notify RSSI 与判定状态。
 *                      写 Control 要求链路加密，明文连接被拒绝。
 *********************************************************************************
 * Copyright (c) 2026
 *******************************************************************************/

/*********************************************************************
 * INCLUDES
 */
#include "CONFIG.h"
#include "hiddev.h"
#include "proxservice.h"

/*********************************************************************
 * MACROS
 */
// Attribute value index positions in proxAttrTbl
#define PROX_SERVICE_IDX              0
#define PROX_CONTROL_VAL_IDX          2
#define PROX_STATUS_VAL_IDX           4
#define PROX_STATUS_CCCD_IDX          5

/*********************************************************************
 * CONSTANTS
 */
/* Service UUID : A5F5AA00-C263-4A0C-8E8F-9C0B7A5D3E01 (little-endian) */
const uint8_t proxServUUID[ATT_UUID_SIZE] = {
    0x01, 0x3E, 0x5D, 0x7A, 0x0B, 0x9C, 0x8F, 0x8E,
    0x0C, 0x4A, 0x63, 0xC2, 0x00, 0xAA, 0xF5, 0xA5};

/* Control characteristic UUID : A5F5AA01-... (little-endian) */
const uint8_t proxControlUUID[ATT_UUID_SIZE] = {
    0x01, 0x3E, 0x5D, 0x7A, 0x0B, 0x9C, 0x8F, 0x8E,
    0x0C, 0x4A, 0x63, 0xC2, 0x01, 0xAA, 0xF5, 0xA5};

/* Status characteristic UUID : A5F5AA02-... (little-endian) */
const uint8_t proxStatusUUID[ATT_UUID_SIZE] = {
    0x02, 0x3E, 0x5D, 0x7A, 0x0B, 0x9C, 0x8F, 0x8E,
    0x0C, 0x4A, 0x63, 0xC2, 0x02, 0xAA, 0xF5, 0xA5};

/*********************************************************************
 * Profile Attributes - variables
 */

// Proximity Service attribute
static const gattAttrType_t proxService = {ATT_UUID_SIZE, proxServUUID};

// Control characteristic
static uint8_t proxControlProps = GATT_PROP_READ | GATT_PROP_WRITE;
static uint8_t proxControl = PROX_CONTROL_DEACTIVATE;

// Status characteristic
static uint8_t proxStatusProps = GATT_PROP_READ | GATT_PROP_NOTIFY;
static uint8_t proxStatus[PROX_STATUS_LEN] = {0, 0, 0};
static gattCharCfg_t proxStatusClientCharCfg[GATT_MAX_NUM_CONN];

/*********************************************************************
 * Profile Attributes - Table
 */
static gattAttribute_t proxAttrTbl[] = {
    // Proximity Service
    // NOTE: 服务声明的 type 是 16-bit primaryServiceUUID (0x2800)，
    // 自定义 128-bit 服务 UUID 放在 pValue (proxService) 里
    {
        {ATT_BT_UUID_SIZE, primaryServiceUUID}, /* type */
        GATT_PERMIT_READ,                       /* permissions */
        0,                                      /* handle */
        (uint8_t *)&proxService                 /* pValue */
    },

    // Control Characteristic Declaration
    {
        {ATT_BT_UUID_SIZE, characterUUID},
        GATT_PERMIT_READ,
        0,
        &proxControlProps},

    // Control Characteristic Value
    {
        {ATT_UUID_SIZE, proxControlUUID},
        GATT_PERMIT_READ | GATT_PERMIT_WRITE,
        0,
        &proxControl},

    // Status Characteristic Declaration
    {
        {ATT_BT_UUID_SIZE, characterUUID},
        GATT_PERMIT_READ,
        0,
        &proxStatusProps},

    // Status Characteristic Value
    {
        {ATT_UUID_SIZE, proxStatusUUID},
        GATT_PERMIT_READ,
        0,
        proxStatus},

    // Status Client Characteristic Configuration
    {
        {ATT_BT_UUID_SIZE, clientCharCfgUUID},
        GATT_PERMIT_READ | GATT_PERMIT_WRITE,
        0,
        (uint8_t *)&proxStatusClientCharCfg},
};

/*********************************************************************
 * LOCAL FUNCTIONS
 */
static bStatus_t proxReadAttrCB(uint16_t connHandle, gattAttribute_t *pAttr,
                                uint8_t *pValue, uint16_t *pLen, uint16_t offset,
                                uint16_t maxLen, uint8_t method);
static bStatus_t proxWriteAttrCB(uint16_t connHandle, gattAttribute_t *pAttr,
                                 uint8_t *pValue, uint16_t len, uint16_t offset,
                                 uint8_t method);

/*********************************************************************
 * PROFILE CALLBACKS
 */
gattServiceCBs_t proxCBs = {
    proxReadAttrCB,  // Read callback
    proxWriteAttrCB, // Write callback
    NULL             // Authorization callback
};

/*********************************************************************
 * @fn      Prox_AddService
 *
 * @brief   Register the Proximity Service with the GATT server.
 *
 * @return  bStatus_t
 */
bStatus_t Prox_AddService(void)
{
    // Initialize Client Characteristic Configuration attributes
    GATTServApp_InitCharCfg(INVALID_CONNHANDLE, proxStatusClientCharCfg);

    // Register GATT attribute list and CBs with GATT Server App
    return GATTServApp_RegisterService(proxAttrTbl,
                                       GATT_NUM_ATTRS(proxAttrTbl),
                                       GATT_MAX_ENCRYPT_KEY_SIZE,
                                       &proxCBs);
}

/*********************************************************************
 * @fn      ProxService_NotifyStatus
 *
 * @brief   Send a Status notification to the monitor (phone) link if
 *          that link exists and has subscribed to notifications.
 */
void ProxService_NotifyStatus(int8_t rssi, uint8_t state, uint8_t flags)
{
    uint16_t monHandle = HidDev_MonConnHandle();

    if(monHandle == GAP_CONNHANDLE_INIT || !linkDB_Up(monHandle))
    {
        return;
    }

    {
        uint16_t value = GATTServApp_ReadCharCfg(monHandle, proxStatusClientCharCfg);
        if(value & GATT_CLIENT_CFG_NOTIFY)
        {
            attHandleValueNoti_t noti;

            noti.pValue = GATT_bm_alloc(monHandle, ATT_HANDLE_VALUE_NOTI,
                                        PROX_STATUS_LEN, NULL, 0);
            if(noti.pValue != NULL)
            {
                noti.handle = proxAttrTbl[PROX_STATUS_VAL_IDX].handle;
                noti.len = PROX_STATUS_LEN;
                noti.pValue[0] = (uint8_t)rssi;
                noti.pValue[1] = state;
                noti.pValue[2] = flags;

                if(GATT_Notification(monHandle, &noti, FALSE) != SUCCESS)
                {
                    GATT_bm_free((gattMsg_t *)&noti, ATT_HANDLE_VALUE_NOTI);
                }
            }
        }
    }
}

/*********************************************************************
 * @fn      proxReadAttrCB
 *
 * @brief   Read callback for Proximity Service attributes.
 */
static bStatus_t proxReadAttrCB(uint16_t connHandle, gattAttribute_t *pAttr,
                                uint8_t *pValue, uint16_t *pLen, uint16_t offset,
                                uint16_t maxLen, uint8_t method)
{
    bStatus_t status = SUCCESS;

    // Make sure it's not a blob operation
    if(offset > 0)
    {
        return (ATT_ERR_ATTR_NOT_LONG);
    }

    if(pAttr->type.len == ATT_UUID_SIZE &&
       tmos_memcmp(pAttr->type.uuid, proxControlUUID, ATT_UUID_SIZE))
    {
        *pLen = 1;
        pValue[0] = proxControl;
    }
    else if(pAttr->type.len == ATT_UUID_SIZE &&
            tmos_memcmp(pAttr->type.uuid, proxStatusUUID, ATT_UUID_SIZE))
    {
        *pLen = PROX_STATUS_LEN;
        tmos_memcpy(pValue, proxStatus, PROX_STATUS_LEN);
    }
    else
    {
        status = ATT_ERR_ATTR_NOT_FOUND;
    }

    return (status);
}

/*********************************************************************
 * @fn      proxWriteAttrCB
 *
 * @brief   Write callback: Control characteristic (activate monitoring)
 *          and Status CCCD (notification subscribe/unsubscribe).
 */
static bStatus_t proxWriteAttrCB(uint16_t connHandle, gattAttribute_t *pAttr,
                                 uint8_t *pValue, uint16_t len, uint16_t offset,
                                 uint8_t method)
{
    bStatus_t status = SUCCESS;

    // Make sure it's not a blob operation
    if(offset > 0)
    {
        return (ATT_ERR_ATTR_NOT_LONG);
    }

    // Status CCCD write
    if(pAttr->type.len == ATT_BT_UUID_SIZE &&
       tmos_memcmp(pAttr->type.uuid, clientCharCfgUUID, ATT_BT_UUID_SIZE))
    {
        status = GATTServApp_ProcessCCCWriteReq(connHandle, pAttr, pValue, len,
                                                offset, GATT_CLIENT_CFG_NOTIFY);
        return (status);
    }

    // Control value write
    if(pAttr->type.len == ATT_UUID_SIZE &&
       tmos_memcmp(pAttr->type.uuid, proxControlUUID, ATT_UUID_SIZE))
    {
        // Require an encrypted link (Just Works bond satisfies this)
        if(!linkDB_State(connHandle, LINK_ENCRYPTED))
        {
            return (ATT_ERR_INSUFFICIENT_ENCRYPT);
        }

        if(len != 1)
        {
            return (ATT_ERR_INVALID_VALUE_SIZE);
        }

        switch(pValue[0])
        {
            case PROX_CONTROL_ACTIVATE:
                // Mark this link as the monitor connection (slot swap safe)
                if(!HidDev_MarkMonitorConnection(connHandle))
                {
                    return (ATT_ERR_INSUFFICIENT_RESOURCES);
                }
                proxControl = PROX_CONTROL_ACTIVATE;
                break;

            case PROX_CONTROL_DEACTIVATE:
                proxControl = PROX_CONTROL_DEACTIVATE;
                // Stop judging this link; slot kept for a later re-activate
                HidDev_DeactivateMonitor();
                break;

            default:
                return (ATT_ERR_INVALID_VALUE);
        }
        return (SUCCESS);
    }

    return (ATT_ERR_ATTR_NOT_FOUND);
}
