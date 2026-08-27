/********************************** (C) COPYRIGHT *******************************
 * File Name          : fp_sm.c
 * Description        : Fingerprint state machine layer (protothread based).
 *                      Wraps fp_proto + fp_uart into a non-blocking state
 *                      machine: ENROLL / VERIFY / DELETE / CLEAR / CANCEL.
 *                      Upper layer drives fp_sm_task() from the main loop
 *                      and receives results via fp_notify_cb_t callback.
 *******************************************************************************/
#include "fp_sm.h"
#include "fp_proto.h"
#include "fp_uart.h"
#include "pt/pt.h"
#include <string.h>

/* ===== 指令码定义 ===== */
#define CMD_AUTO_ENROLL    0x31
#define CMD_AUTO_IDENTIFY  0x32
#define CMD_DELETE_CHAR    0x0C
#define CMD_EMPTY          0x0D
#define CMD_CANCEL         0x30

/* ===== 接收应答缓冲结构体 ===== */
typedef struct {
    uint8_t  data[FP_MAX_DATA];
    uint16_t len;
    uint8_t  pid;
    BOOL     ready;       /* 有新包待消费 */
    uint32_t timestamp;   /* 接收时间戳 */
} fp_rx_t;

/* ===== 全局状态变量 ===== */
static struct pt s_pt;          /* protothread 上下文 */
static fp_state_t s_state;      /* 当前状态 */
static fp_notify_cb_t s_notify; /* 通知回调 */
static uint16_t s_target_id;    /* 目标 PageID（注册/删除用） */
static fp_rx_t s_rx;            /* 接收应答缓冲 */
static uint32_t s_wait_start;   /* 等待起始时间（用于超时） */
static uint32_t s_tick;         /* 全局 tick 计数器（5ms 周期递增） */
static fp_state_t s_pending_state; /* 取消后要切换到的状态, FP_IDLE=无(回到验证) */
static uint16_t   s_pending_id;    /* 待执行操作的目标 ID (enroll/delete 用) */

/* ===== fp_proto 回调函数（注册到 fp_proto） ===== */
static void on_packet_cb(uint8_t pid, const uint8_t *data, uint16_t len) {
    if (len > FP_MAX_DATA) len = FP_MAX_DATA;
    memcpy(s_rx.data, data, len);
    s_rx.len = len;
    s_rx.pid = pid;
    s_rx.ready = TRUE;
    /* timestamp 由 fp_sm_task 更新 tick 时设置, 这里也可以用自增计数器 */
}

/* ===== 等待应答的辅助宏（在 protothread 内使用） ===== */
/* 等待 s_rx.ready 或超时, 返回 PT_WAITING 让出 */
#define FP_WAIT_RESPONSE(pt, timeout_ms) \
    do { \
        s_wait_start = s_tick; \
        PT_WAIT_UNTIL((pt), s_rx.ready || (s_tick - s_wait_start) > (timeout_ms / 5)); \
        if (!s_rx.ready) { \
            if (s_notify) s_notify(FP_MSG_TIMEOUT, 0, 0); \
            s_state = FP_IDLE; \
            PT_EXIT((pt)); \
        } \
    } while(0)

/* ===== ENROLL protothread（自动注册） ===== */
static PT_THREAD(pt_enroll(struct pt *pt)) {
    static uint8_t buf[16];
    static uint8_t params[5];  /* ID(2) + Times(1) + Param(2) */
    uint16_t pkt_len;
    uint8_t confirm, step, seq;

    PT_BEGIN(pt);

    /* 组装 0x31 指令 */
    params[0] = (uint8_t)(s_target_id >> 8);
    params[1] = (uint8_t)(s_target_id & 0xFF);
    params[2] = FP_ENROLL_TIMES;
    params[3] = (uint8_t)(FP_ENROLL_PARAM >> 8);
    params[4] = (uint8_t)(FP_ENROLL_PARAM & 0xFF);
    pkt_len = fp_proto_build_cmd(buf, CMD_AUTO_ENROLL, params, 5);
    fp_uart_send(buf, pkt_len);

    /* 循环处理多步骤应答 */
    while (1) {
        s_rx.ready = FALSE;
        FP_WAIT_RESPONSE(pt, FP_WAIT_ENROLL_TIMEOUT_MS);

        /* 解析应答: data[0]=确认码, data[1]=步骤码, data[2]=Param2 */
        confirm = s_rx.data[0];
        step = s_rx.data[1];
        seq = s_rx.data[2];

        if (confirm == 0x00) {
            /* 成功步骤 */
            if (step == 0x00) {
                /* 合法性检测通过 */
                if (s_notify) s_notify(FP_MSG_ENROLL_STEP, step, seq);
            } else if (step == 0x01) {
                /* 采图成功 */
                if (s_notify) s_notify(FP_MSG_ENROLL_STEP, step, seq);
            } else if (step == 0x02) {
                /* 生成特征成功 */
                if (s_notify) s_notify(FP_MSG_ENROLL_STEP, step, seq);
            } else if (step == 0x03) {
                /* 手指离开提示 */
                if (s_notify) s_notify(FP_MSG_ENROLL_STEP, step, seq);
            } else if (step == 0x04) {
                /* 合并模板成功 */
                if (s_notify) s_notify(FP_MSG_ENROLL_STEP, step, seq);
            } else if (step == 0x05) {
                /* 重复检测通过 */
                if (s_notify) s_notify(FP_MSG_ENROLL_STEP, step, seq);
            } else if (step == 0x06) {
                /* 存储成功 = 注册完成! */
                if (s_notify) s_notify(FP_MSG_ENROLL_OK, s_target_id, 0);
                break;  /* 退出循环, 回到 IDLE */
            }
        } else {
            /* 失败步骤 */
            if (s_notify) s_notify(FP_MSG_ENROLL_FAIL, confirm, step);
            break;
        }
    }

    s_state = FP_IDLE;
    PT_END(pt);
}

/* ===== VERIFY protothread（1:N 自动验证） ===== */
static PT_THREAD(pt_verify(struct pt *pt)) {
    static uint8_t buf[16];
    static uint8_t params[5];  /* ScoreLevel(1) + TargetID(2) + Param(2) */
    uint16_t pkt_len;
    uint8_t confirm, step;
    uint16_t page_id, score;

    PT_BEGIN(pt);

    params[0] = FP_VERIFY_SCORE_LEVEL;
    params[1] = (uint8_t)(FP_SEARCH_ALL >> 8);    /* 0xFF */
    params[2] = (uint8_t)(FP_SEARCH_ALL & 0xFF);   /* 0xFF */
    params[3] = 0x04;  /* Param: 不要求返回步骤 */
    params[4] = 0x00;
    pkt_len = fp_proto_build_cmd(buf, CMD_AUTO_IDENTIFY, params, 5);
    fp_uart_send(buf, pkt_len);

    while (1) {
        s_rx.ready = FALSE;
        FP_WAIT_RESPONSE(pt, FP_WAIT_VERIFY_TIMEOUT_MS);

        /* 应答格式: data[0]=确认码, data[1]=步骤, data[2..3]=PageID, data[4..5]=Score */
        confirm = s_rx.data[0];
        step = s_rx.data[1];

        if (confirm == 0x00) {
            if (step == 0x00) {
                /* 合法性检测通过, 继续 */
            } else if (step == 0x01) {
                /* 采图成功 */
            } else if (step == 0x05) {
                /* 比对结果 */
                page_id = ((uint16_t)s_rx.data[2] << 8) | s_rx.data[3];
                score = ((uint16_t)s_rx.data[4] << 8) | s_rx.data[5];
                if (page_id != 0 || score != 0) {
                    if (s_notify) s_notify(FP_MSG_VERIFY_OK, page_id, score);
                } else {
                    if (s_notify) s_notify(FP_MSG_VERIFY_FAIL, 0x09, 0);  /* 未搜索到 */
                }
                break;
            }
        } else {
            /* 失败: 0x09未搜到 / 0x17残留 / 0x24库空 等 */
            if (s_notify) s_notify(FP_MSG_VERIFY_FAIL, confirm, 0);
            break;
        }
    }

    s_state = FP_IDLE;
    PT_END(pt);
}

/* ===== DELETE_ONE protothread（删除指定 ID） ===== */
static PT_THREAD(pt_delete(struct pt *pt)) {
    static uint8_t buf[16];
    static uint8_t params[4];  /* PageID(2) + N(2) */
    uint16_t pkt_len;

    PT_BEGIN(pt);

    params[0] = (uint8_t)(s_target_id >> 8);
    params[1] = (uint8_t)(s_target_id & 0xFF);
    params[2] = 0x00;  /* N=1 (高字节) */
    params[3] = 0x01;  /* N=1 (低字节) */
    pkt_len = fp_proto_build_cmd(buf, CMD_DELETE_CHAR, params, 4);
    fp_uart_send(buf, pkt_len);

    s_rx.ready = FALSE;
    FP_WAIT_RESPONSE(pt, FP_WAIT_TIMEOUT_MS);

    if (s_rx.data[0] == 0x00) {
        if (s_notify) s_notify(FP_MSG_DELETE_OK, 0, 0);
    } else {
        if (s_notify) s_notify(FP_MSG_DELETE_FAIL, s_rx.data[0], 0);
    }

    s_state = FP_IDLE;
    PT_END(pt);
}

/* ===== CLEAR_ALL protothread（清空全部） ===== */
static PT_THREAD(pt_clear(struct pt *pt)) {
    static uint8_t buf[16];
    uint16_t pkt_len;

    PT_BEGIN(pt);

    pkt_len = fp_proto_build_cmd(buf, CMD_EMPTY, NULL, 0);
    fp_uart_send(buf, pkt_len);

    s_rx.ready = FALSE;
    FP_WAIT_RESPONSE(pt, FP_WAIT_TIMEOUT_MS);

    if (s_rx.data[0] == 0x00) {
        if (s_notify) s_notify(FP_MSG_CLEAR_OK, 0, 0);
    } else {
        if (s_notify) s_notify(FP_MSG_CLEAR_FAIL, s_rx.data[0], 0);
    }

    s_state = FP_IDLE;
    PT_END(pt);
}

/* ===== CANCEL protothread（取消） ===== */
static PT_THREAD(pt_cancel(struct pt *pt)) {
    static uint8_t buf[16];
    uint16_t pkt_len;

    PT_BEGIN(pt);

    pkt_len = fp_proto_build_cmd(buf, CMD_CANCEL, NULL, 0);
    fp_uart_send(buf, pkt_len);

    s_rx.ready = FALSE;
    FP_WAIT_RESPONSE(pt, FP_WAIT_TIMEOUT_MS);

    /* 无论结果都回到 IDLE */
    if (s_notify) s_notify(FP_MSG_CANCELLED, 0, 0);
    s_state = FP_IDLE;
    PT_END(pt);
}

/* ===== 是否处于不可打断的操作中 ===== */
/* VERIFY 可被打断 (注册/删除/清空/取消都会先抢占它), 故不算 busy */
static BOOL fp_sm_busy(void) {
    return (s_state == FP_ENROLL || s_state == FP_DELETE_ONE ||
            s_state == FP_CLEAR_ALL || s_state == FP_CANCEL);
}

/* ===== 主任务函数：由主循环周期调用, 推进状态机 ===== */
void fp_sm_task(void) {
    s_tick++;  /* 全局 tick 递增 (5ms周期) */

    /* IDLE 状态: 自动启动验证 (开机默认 / 操作结束 / 超时后 均回到这里) */
    if (s_state == FP_IDLE) {
        s_state = FP_VERIFY;
        PT_INIT(&s_pt);
    }

    switch (s_state) {
    case FP_ENROLL:
        PT_SCHEDULE(pt_enroll(&s_pt));
        break;
    case FP_VERIFY:
        PT_SCHEDULE(pt_verify(&s_pt));
        break;
    case FP_DELETE_ONE:
        PT_SCHEDULE(pt_delete(&s_pt));
        break;
    case FP_CLEAR_ALL:
        PT_SCHEDULE(pt_clear(&s_pt));
        break;
    case FP_CANCEL:
        PT_SCHEDULE(pt_cancel(&s_pt));
        break;
    default:
        break;  /* 不会到达: IDLE 已在上面转为 VERIFY */
    }
}

/* ===== 状态切换 API ===== */
/* 注: VERIFY 状态可被打断 (回到默认验证前先执行请求的操作);
 * 仅当处于 ENROLL/DELETE/CLEAR/CANCEL 等不可打断的操作时拒绝。 */
BOOL fp_sm_enroll(uint16_t page_id) {
    if (fp_sm_busy()) {
        if (s_notify) s_notify(FP_MSG_BUSY, 0, 0);
        return FALSE;
    }
    s_target_id = page_id;
    s_state = FP_ENROLL;
    PT_INIT(&s_pt);
    return TRUE;
}

BOOL fp_sm_verify(void) {
    if (fp_sm_busy()) {
        if (s_notify) s_notify(FP_MSG_BUSY, 0, 0);
        return FALSE;
    }
    s_state = FP_VERIFY;
    PT_INIT(&s_pt);
    return TRUE;
}

BOOL fp_sm_delete(uint16_t page_id) {
    if (fp_sm_busy()) {
        if (s_notify) s_notify(FP_MSG_BUSY, 0, 0);
        return FALSE;
    }
    s_target_id = page_id;
    s_state = FP_DELETE_ONE;
    PT_INIT(&s_pt);
    return TRUE;
}

BOOL fp_sm_clear(void) {
    if (fp_sm_busy()) {
        if (s_notify) s_notify(FP_MSG_BUSY, 0, 0);
        return FALSE;
    }
    s_state = FP_CLEAR_ALL;
    PT_INIT(&s_pt);
    return TRUE;
}

BOOL fp_sm_cancel(void) {
    if (fp_sm_busy()) {
        if (s_notify) s_notify(FP_MSG_BUSY, 0, 0);
        return FALSE;
    }
    s_state = FP_CANCEL;
    PT_INIT(&s_pt);
    return TRUE;
}

fp_state_t fp_sm_get_state(void) {
    return s_state;
}

void fp_sm_set_notify_cb(fp_notify_cb_t cb) {
    s_notify = cb;
}

/* ===== 初始化 ===== */
void fp_sm_init(void) {
    s_state = FP_IDLE;
    s_notify = NULL;
    s_target_id = 0;
    s_tick = 0;
    memset(&s_rx, 0, sizeof(s_rx));
    fp_proto_set_callback(on_packet_cb);
}
