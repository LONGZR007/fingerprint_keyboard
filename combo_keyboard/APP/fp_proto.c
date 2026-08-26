/********************************** (C) COPYRIGHT *******************************
 * File Name          : fp_proto.c
 * Description        : 指纹模组通信协议解析层
 *                      - 单字节 switch 状态机解析数据包
 *                      - 包格式: EF01 + addr(4B) + PID(1B) + len(2B,BE) + data(NB) + chk(2B)
 *                      - 校验和 = (PID + len + data) 之和取低16位
 *                      - 提供指令包组装辅助函数 fp_proto_build_cmd()
 *******************************************************************************/

#include "fp_proto.h"
#include <string.h>

/* =======================================================================
 * 静态状态变量
 * ===================================================================== */
static fp_parse_state_t s_state;          /* 解析状态 */
static uint8_t  s_addr[4];                /* 设备地址缓冲 */
static uint8_t  s_addr_idx;               /* 地址字节计数 */
static uint8_t  s_pid;                    /* 当前包标识 */
static uint16_t s_pkt_len;                /* 包长度域(大端值) */
static uint8_t  s_data[FP_MAX_DATA];      /* 数据缓冲 */
static uint16_t s_data_idx;               /* 已收数据字节计数 */
static uint16_t s_data_len;               /* 期望数据字节数 = pkt_len - 3 */
static uint8_t  s_chk_hi;                /* 接收的校验和高字节 */
static uint8_t  s_chk_lo;                /* 接收的校验和低字节 */
static uint16_t s_chk_sum;                /* 计算的校验和累加器 */

static fp_packet_cb_t s_callback;         /* 完整包回调 */
static BOOL    s_packet_ready;            /* 完整包就绪标志 */

/* 超时检测：fp_proto_task() 每次调用递增一次计数器。
 * 若 fp_proto_task 由 5ms 周期任务调用，100 次 ≈ 500ms。 */
#ifndef FP_PROTO_TIMEOUT_TICKS
#define FP_PROTO_TIMEOUT_TICKS   100
#endif
static uint32_t s_tick_counter;          /* 任务调用计数器(自增tick) */
static uint32_t s_last_tick;             /* 上次接收字节时的 tick */

/* =======================================================================
 * 内部：复位状态机到 HEAD0
 * ===================================================================== */
static void fp_reset_state(void)
{
    s_state     = FP_ST_HEAD0;
    s_addr_idx  = 0;
    s_data_idx  = 0;
    s_data_len  = 0;
    s_chk_sum   = 0;
}

/* =======================================================================
 * 初始化
 * ===================================================================== */
void fp_proto_init(void)
{
    s_state       = FP_ST_HEAD0;
    s_addr_idx    = 0;
    s_pid         = 0;
    s_pkt_len     = 0;
    s_data_idx    = 0;
    s_data_len    = 0;
    s_chk_hi      = 0;
    s_chk_lo      = 0;
    s_chk_sum     = 0;
    s_callback    = NULL;
    s_packet_ready = FALSE;
    s_tick_counter = 0;
    s_last_tick    = 0;
}

/* =======================================================================
 * 设置完整包回调
 * ===================================================================== */
void fp_proto_set_callback(fp_packet_cb_t cb)
{
    s_callback = cb;
}

/* =======================================================================
 * 查询是否有完整包待处理
 * ===================================================================== */
BOOL fp_proto_packet_ready(void)
{
    return s_packet_ready;
}

/* =======================================================================
 * 接收单字节 —— switch 状态机
 * 由 fp_uart_drain() 逐字节调用
 * ===================================================================== */
void fp_proto_rx_byte(uint8_t b)
{
    /* 记录本次接收的活动时间戳 */
    s_last_tick = s_tick_counter;

    switch (s_state) {

    case FP_ST_HEAD0:
        if (b == FP_HEAD0) {
            s_state = FP_ST_HEAD1;
        }
        /* 否则停在 HEAD0 */
        break;

    case FP_ST_HEAD1:
        if (b == FP_HEAD1) {
            s_addr_idx = 0;
            s_chk_sum  = 0;        /* 校验和从 PID 开始累加 */
            s_state    = FP_ST_ADDR;
        } else {
            s_state = FP_ST_HEAD0;
        }
        break;

    case FP_ST_ADDR:
        /* 存入地址缓冲(校验和不含地址，仅存储) */
        s_addr[s_addr_idx++] = b;
        if (s_addr_idx >= 4) {
            s_state = FP_ST_PID;
        }
        break;

    case FP_ST_PID:
        s_pid = b;
        s_chk_sum += b;            /* PID 是校验和起点 */
        s_state = FP_ST_LEN_HI;
        break;

    case FP_ST_LEN_HI:
        s_pkt_len = (uint16_t)b << 8;
        s_chk_sum += b;
        s_state = FP_ST_LEN_LO;
        break;

    case FP_ST_LEN_LO:
        s_pkt_len |= b;
        s_chk_sum += b;
        /* 数据长度 = 包长度 - 3 (PID 1B + 校验和 2B) */
        if (s_pkt_len < 3) {
            fp_reset_state();       /* 非法长度 */
            break;
        }
        s_data_len = (uint16_t)(s_pkt_len - 3);
        if (s_data_len > FP_MAX_DATA) {
            fp_reset_state();       /* 超出缓冲，丢弃 */
            break;
        }
        s_data_idx = 0;
        if (s_data_len == 0) {
            s_state = FP_ST_CHK_HI; /* 无数据，直接收校验和 */
        } else {
            s_state = FP_ST_DATA;
        }
        break;

    case FP_ST_DATA:
        s_data[s_data_idx++] = b;
        s_chk_sum += b;
        if (s_data_idx >= s_data_len) {
            s_state = FP_ST_CHK_HI;
        }
        break;

    case FP_ST_CHK_HI:
        s_chk_hi = b;
        s_state = FP_ST_CHK_LO;
        break;

    case FP_ST_CHK_LO:
        s_chk_lo = b;
        {
            uint16_t recv_chk = (uint16_t)(((uint16_t)s_chk_hi << 8) | s_chk_lo);
            if (recv_chk == (uint16_t)(s_chk_sum & 0xFFFF)) {
                /* 校验通过：通知有完整包 */
                s_packet_ready = TRUE;
                if (s_callback) {
                    s_callback(s_pid, s_data, s_data_len);
                }
            }
            /* 无论通过与否，复位到 HEAD0 等待下一包 */
        }
        fp_reset_state();
        break;

    default:
        fp_reset_state();
        break;
    }
}

/* =======================================================================
 * 任务函数：超时复位检查（由主循环调用）
 * ===================================================================== */
void fp_proto_task(void)
{
    s_tick_counter++;

    /* 若不在 HEAD0 且距上次接收超过阈值，复位状态机 */
    if (s_state != FP_ST_HEAD0) {
        if ((s_tick_counter - s_last_tick) > FP_PROTO_TIMEOUT_TICKS) {
            fp_reset_state();
        }
    }
}

/* =======================================================================
 * 计算校验和：从 data[0] 到 data[len-1] 求和取低16位
 * ===================================================================== */
uint16_t fp_proto_checksum(const uint8_t *data, uint16_t len)
{
    uint16_t sum = 0;
    uint16_t i;
    for (i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

/* =======================================================================
 * 组装指令包
 * 格式: EF01 + addr(FFFFFFFF) + PID(01) + len(BE) + cmd + params + chk(BE)
 * 包长度 = 1(PID) + 1(cmd) + param_len + 2(校验和) = param_len + 4
 * 校验和范围: PID + len(2B) + cmd + params
 * 返回完整指令包总长度
 * ===================================================================== */
uint16_t fp_proto_build_cmd(uint8_t *buf, uint8_t cmd, const uint8_t *params, uint8_t param_len)
{
    uint16_t pkt_len = (uint16_t)(1 + 1 + param_len + 2);  /* PID + cmd + params + chk */
    uint16_t chk;
    uint16_t idx = 0;

    /* 包头 */
    buf[idx++] = FP_HEAD0;
    buf[idx++] = FP_HEAD1;

    /* 设备地址(广播) */
    buf[idx++] = 0xFF;
    buf[idx++] = 0xFF;
    buf[idx++] = 0xFF;
    buf[idx++] = 0xFF;

    /* 包标识: 命令包 */
    buf[idx++] = FP_PID_CMD;

    /* 包长度(大端) */
    buf[idx++] = (uint8_t)(pkt_len >> 8);
    buf[idx++] = (uint8_t)(pkt_len & 0xFF);

    /* 数据区: 指令码 + 参数 */
    buf[idx++] = cmd;
    if (params && param_len > 0) {
        memcpy(&buf[idx], params, param_len);
        idx += param_len;
    }

    /* 校验和: 从 PID(buf[6]) 到最后一个数据字节，求和取低16位 */
    chk = fp_proto_checksum(&buf[6], (uint16_t)(idx - 6));
    buf[idx++] = (uint8_t)(chk >> 8);
    buf[idx++] = (uint8_t)(chk & 0xFF);

    return idx;
}
