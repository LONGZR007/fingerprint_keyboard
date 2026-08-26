/********************************** (C) COPYRIGHT *******************************
 * File Name          : fp_sm.h
 * Description        : Fingerprint state machine layer (protothread based).
 *                      Wraps fp_proto + fp_uart into a non-blocking state
 *                      machine: ENROLL / VERIFY / DELETE / CLEAR / CANCEL.
 *                      Upper layer drives fp_sm_task() from the main loop
 *                      and receives results via fp_notify_cb_t callback.
 *******************************************************************************/
#ifndef __FP_SM_H__
#define __FP_SM_H__
#include "CH58x_common.h"
#include "../pt/pt.h"   /* protothread 库在 APP/pt/ 下 */

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 状态机状态 ===== */
typedef enum {
    FP_IDLE = 0,
    FP_ENROLL,       /* 自动注册 */
    FP_VERIFY,       /* 自动验证 */
    FP_DELETE_ONE,   /* 删除指定 */
    FP_CLEAR_ALL,    /* 清空全部 */
    FP_CANCEL        /* 取消 */
} fp_state_t;

/* ===== 消息通知类型 ===== */
typedef enum {
    FP_MSG_ENROLL_OK = 0,    /* 注册成功, param1=PageID */
    FP_MSG_ENROLL_STEP,      /* 注册步骤, param1=步骤码, param2=录入次数 */
    FP_MSG_ENROLL_FAIL,      /* 注册失败, param1=确认码 */
    FP_MSG_VERIFY_OK,        /* 验证成功, param1=PageID, param2=得分 */
    FP_MSG_VERIFY_FAIL,      /* 验证失败, param1=确认码 */
    FP_MSG_DELETE_OK,        /* 删除成功 */
    FP_MSG_DELETE_FAIL,      /* 删除失败, param1=确认码 */
    FP_MSG_CLEAR_OK,         /* 清空成功 */
    FP_MSG_CLEAR_FAIL,       /* 清空失败, param1=确认码 */
    FP_MSG_TIMEOUT,          /* 超时 */
    FP_MSG_CANCELLED,        /* 已取消 */
    FP_MSG_BUSY              /* 忙状态拒绝 */
} fp_msg_t;

/* ===== 注册配置宏（方便修改） ===== */
/* 录入次数: 大面积=2, 中面积=3, 小面积>=5 */
#ifndef FP_ENROLL_TIMES
#define FP_ENROLL_TIMES       3
#endif

/* 注册 Param 位图:
 *   bit0=1: 采图成功后 LED 灭
 *   bit2=0: 要求返回关键步骤
 *   bit3=1: 允许覆盖已有 ID
 *   bit4=0: 允许重复注册
 *   bit5=0: 每次采集后要求手指离开
 * 默认值 = (1<<0) | (1<<3) = 0x09 */
#ifndef FP_ENROLL_PARAM
#define FP_ENROLL_PARAM       0x09
#endif

/* 验证比对分数等级 1~5 */
#ifndef FP_VERIFY_SCORE_LEVEL
#define FP_VERIFY_SCORE_LEVEL 2
#endif

/* 等待应答超时(ms) */
#ifndef FP_WAIT_TIMEOUT_MS
#define FP_WAIT_TIMEOUT_MS    5000
#endif

/* 1:N 全库搜索的 TargetID */
#define FP_SEARCH_ALL         0xFFFF

/* 回调类型: msg=消息类型, param1/param2=参数 */
typedef void (*fp_notify_cb_t)(fp_msg_t msg, uint16_t param1, uint16_t param2);

/* ===== 公开 API ===== */
void fp_sm_init(void);
void fp_sm_task(void);              /* 由主循环周期调用, 推进状态机 */
BOOL fp_sm_enroll(uint16_t page_id); /* 启动注册, 返回是否成功启动 */
BOOL fp_sm_verify(void);             /* 启动 1:N 验证 */
BOOL fp_sm_delete(uint16_t page_id); /* 删除指定 ID */
BOOL fp_sm_clear(void);              /* 清空全部 */
BOOL fp_sm_cancel(void);             /* 取消当前操作 */
fp_state_t fp_sm_get_state(void);    /* 查询当前状态 */
void fp_sm_set_notify_cb(fp_notify_cb_t cb); /* 设置通知回调 */

#ifdef __cplusplus
}
#endif
#endif
