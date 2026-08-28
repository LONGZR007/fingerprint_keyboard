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
#define CMD_FINGER_PRESENT 0x01   /* 查询是否有手指: 确认码 0=有, 2=无, 其他=错误 */
#define CMD_AUTO_ENROLL    0x31
#define CMD_AUTO_IDENTIFY  0x32
#define CMD_DELETE_CHAR    0x0C
#define CMD_EMPTY          0x0D
#define CMD_CANCEL         0x30
#define CMD_BLN_AUTO_MANUAL 0x60 /* 呼吸灯自动/手动切换: 参数 0xFF=自动, 0x00=手动 */
#define CMD_BLN_CONTROL     0x3C /* 呼吸灯控制: 功能码(1B)+起始颜色(1B)+结束颜色(1B)+循环次数(1B) */

/* 三色灯颜色位: bit0=蓝, bit1=绿, bit2=红 */
#define FP_LED_GREEN        0x02
#define FP_LED_RED          0x04

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
static uint16_t   s_pending_id;    /* 待执行操作的目标 ID (enroll/delete 用) */
static BOOL       s_bln_mode;      /* 呼吸灯模式: TRUE=自动(0xFF), FALSE=手动(0x00) */

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
                break;  /* 退出循环 */
            }
        } else {
            /* 失败步骤 */
            if (s_notify) s_notify(FP_MSG_ENROLL_FAIL, confirm, step);
            break;
        }
    }

    PT_END(pt);  /* 结束由 fp_sm_task 统一切回 FP_IDLE */
}

/* ===== VERIFY protothread（1:N 自动验证） ===== */
static PT_THREAD(pt_verify(struct pt *pt)) {
    static uint8_t buf[16];
    static uint8_t params[5];  /* ScoreLevel(1) + TargetID(2) + Param(2) */
    static uint8_t bln_color;  /* 需亮的灯色(跨 yield 保留): 0=不亮, FP_LED_GREEN/RED */
    uint16_t pkt_len;
    uint8_t confirm, step;
    uint16_t page_id, score;

    PT_BEGIN(pt);

    bln_color = 0;  /* PT_EXIT 重进(全新流程)时复位; resume 时不执行 */

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
                    bln_color = FP_LED_GREEN;  /* 验证成功: 稍后绿灯呼吸一个循环 */
                } else {
                    if (s_notify) s_notify(FP_MSG_VERIFY_FAIL, 0x09, 0);  /* 未搜索到 */
                    bln_color = FP_LED_RED;    /* 未搜索到: 稍后红灯呼吸一个循环 */
                }
                break;
            }
        } else if (confirm == 0x26) {
            break;    // 超时不通知
        } else {
            /* 失败: 0x09未搜到 / 0x17残留 / 0x24库空 等 */
            if (s_notify) s_notify(FP_MSG_VERIFY_FAIL, confirm, 0);
            bln_color = FP_LED_RED;            /* 验证失败: 稍后红灯呼吸一个循环 */
            break;
        }
    }

    /* 验证结果灯: 统一在进入手指离开轮询之前亮灯, 并等待 0x3C 应答 */
    if (bln_color != 0) {
        params[0] = 0x01;      /* 功能码: 呼吸灯 */
        params[1] = bln_color; /* 起始颜色 */
        params[2] = 0x00; /* 结束颜色 */
        params[3] = 0x01;      /* 循环次数: 1 次 */
        pkt_len = fp_proto_build_cmd(buf, CMD_BLN_CONTROL, params, 4);
        fp_uart_send(buf, pkt_len);

        /* 等待应答; 超时不退出, 灯指令失败不影响验证流程 */
        s_rx.ready = FALSE;
        s_wait_start = s_tick;
        PT_WAIT_UNTIL(pt, s_rx.ready || (s_tick - s_wait_start) > (FP_WAIT_TIMEOUT_MS / 5));
        bln_color = 0;
    }

    // 等待手指离开: 循环发送 0x01 查询指令, 直到确认码 != 0
    while (1) {
        s_rx.ready = FALSE;
        pkt_len = fp_proto_build_cmd(buf, CMD_FINGER_PRESENT, NULL, 0);
        fp_uart_send(buf, pkt_len);

        FP_WAIT_RESPONSE(pt, FP_WAIT_TIMEOUT_MS);

        confirm = s_rx.data[0];
        if (confirm != 0x00) {
            /* 0x02=无手指, 其他=错误: 均视为手指已离开 */
            break;
        }
        /* 0x00=手指仍在, 继续查询 */
    }

    // s_state = FP_IDLE;   // 这里不切换状态, 继续验证下一枚指纹
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

    PT_END(pt);  /* 结束由 fp_sm_task 统一切回 FP_IDLE */
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

    PT_END(pt);  /* 结束由 fp_sm_task 统一切回 FP_IDLE */
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

    /* 无论结果都结束, 由 fp_sm_task 统一切回 IDLE */
    if (s_notify) s_notify(FP_MSG_CANCELLED, 0, 0);
    PT_END(pt);
}

/* ===== BLN_SET protothread（呼吸灯自动/手动模式设置） ===== */
/* 发送 0x60 指令后等待应答; 超时/失败/成功都回到 IDLE。 */
static PT_THREAD(pt_bln_set(struct pt *pt)) {
    static uint8_t buf[16];
    static uint8_t params[1];  /* 0xFF=自动, 0x00=手动 */
    uint16_t pkt_len;
    uint8_t confirm;

    PT_BEGIN(pt);

    params[0] = s_bln_mode ? 0xFF : 0x00;
    pkt_len = fp_proto_build_cmd(buf, CMD_BLN_AUTO_MANUAL, params, 1);
    fp_uart_send(buf, pkt_len);

    s_rx.ready = FALSE;
    FP_WAIT_RESPONSE(pt, FP_WAIT_TIMEOUT_MS);

    confirm = s_rx.data[0];
    if (confirm == 0x00) {
        if (s_notify) s_notify(FP_MSG_BLN_OK, params[0], 0);
    } else {
        if (s_notify) s_notify(FP_MSG_BLN_FAIL, confirm, 0);
    }

    PT_END(pt);  /* 结束由 fp_sm_task 统一切回 FP_IDLE */
}

/* ===== 主任务函数：由主循环周期调用, 推进状态机 ===== */
void fp_sm_task(void) {
    s_tick++;  /* 全局 tick 递增 (5ms周期) */

    switch (s_state) {
    case FP_ENROLL:
        if (!PT_SCHEDULE(pt_enroll(&s_pt))) s_state = FP_IDLE;
        break;
    case FP_VERIFY:
        /* 验证为常驻轮询, 即使 protothread 结束也不切回 IDLE */
        PT_SCHEDULE(pt_verify(&s_pt));
        break;
    case FP_DELETE_ONE:
        if (!PT_SCHEDULE(pt_delete(&s_pt))) s_state = FP_IDLE;
        break;
    case FP_CLEAR_ALL:
        if (!PT_SCHEDULE(pt_clear(&s_pt))) s_state = FP_IDLE;
        break;
    case FP_CANCEL:
        if (!PT_SCHEDULE(pt_cancel(&s_pt))) s_state = FP_IDLE;
        break;
    case FP_BLN_SET:
        if (!PT_SCHEDULE(pt_bln_set(&s_pt))) s_state = FP_IDLE;
        break;
    default:
        break;  /* IDLE, 不调度 */
    }
}

/* ===== 状态切换 API ===== */
BOOL fp_sm_enroll(uint16_t page_id) {
    if (s_state != FP_IDLE) {
        if (s_notify) s_notify(FP_MSG_BUSY, 0, 0);
        return FALSE;
    }
    s_target_id = page_id;
    s_state = FP_ENROLL;
    PT_INIT(&s_pt);
    return TRUE;
}

BOOL fp_sm_verify(void) {
    if (s_state != FP_IDLE) {
        if (s_notify) s_notify(FP_MSG_BUSY, 0, 0);
        return FALSE;
    }
    s_state = FP_VERIFY;
    PT_INIT(&s_pt);
    return TRUE;
}

BOOL fp_sm_delete(uint16_t page_id) {
    if (s_state != FP_IDLE) {
        if (s_notify) s_notify(FP_MSG_BUSY, 0, 0);
        return FALSE;
    }
    s_target_id = page_id;
    s_state = FP_DELETE_ONE;
    PT_INIT(&s_pt);
    return TRUE;
}

BOOL fp_sm_clear(void) {
    if (s_state != FP_IDLE) {
        if (s_notify) s_notify(FP_MSG_BUSY, 0, 0);
        return FALSE;
    }
    s_state = FP_CLEAR_ALL;
    PT_INIT(&s_pt);
    return TRUE;
}

BOOL fp_sm_cancel(void) {
    s_state = FP_CANCEL;
    PT_INIT(&s_pt);
    return TRUE;
}

BOOL fp_sm_set_bln_mode(BOOL auto_mode) {
    /* 仅允许在空闲状态发起, 其他操作进行中拒绝 */
    if (s_state != FP_IDLE) {
        if (s_notify) s_notify(FP_MSG_BUSY, 0, 0);
        return FALSE;
    }
    s_bln_mode = auto_mode;
    s_state = FP_BLN_SET;
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
    s_state = FP_VERIFY;
    s_notify = NULL;
    s_target_id = 0;
    s_tick = 0;
    s_bln_mode = TRUE;   /* 默认自动模式 */
    memset(&s_rx, 0, sizeof(s_rx));
    fp_proto_set_callback(on_packet_cb);

    /* PA14: 推挽输出 20mA 驱动能力, 输出高电平 */
    GPIOA_ModeCfg(GPIO_Pin_14, GPIO_ModeOut_PP_20mA);
    GPIOA_SetBits(GPIO_Pin_14);
}
