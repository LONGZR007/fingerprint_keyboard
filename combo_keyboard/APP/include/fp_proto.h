#ifndef __FP_PROTO_H__
#define __FP_PROTO_H__
#include "CH58x_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 包头 */
#define FP_HEAD0   0xEF
#define FP_HEAD1   0x01

/* 包标识 */
#define FP_PID_CMD     0x01   /* 命令包 */
#define FP_PID_ACK     0x07   /* 应答包 */
#define FP_PID_DATA    0x02   /* 数据包(有后续) */
#define FP_PID_END     0x08   /* 结束数据包 */

/* 解析状态 */
typedef enum {
    FP_ST_HEAD0,    /* 等待 0xEF */
    FP_ST_HEAD1,    /* 等待 0x01 */
    FP_ST_ADDR,     /* 设备地址 4 字节 */
    FP_ST_PID,      /* 包标识 */
    FP_ST_LEN_HI,   /* 包长度高字节 */
    FP_ST_LEN_LO,   /* 包长度低字节 */
    FP_ST_DATA,     /* 数据 */
    FP_ST_CHK_HI,   /* 校验和高字节 */
    FP_ST_CHK_LO    /* 校验和低字节 */
} fp_parse_state_t;

/* 最大数据长度（包长度域表示的数据+确认码等，512足够） */
#ifndef FP_MAX_DATA
#define FP_MAX_DATA    512
#endif

/* 完整包回调：pid=包标识, data=数据区(含确认码等), len=数据长度 */
typedef void (*fp_packet_cb_t)(uint8_t pid, const uint8_t *data, uint16_t len);

/* 接收单字节（由 fp_uart_drain 调用） */
void fp_proto_rx_byte(uint8_t b);

/* 任务函数：检查超时复位（由主循环调用） */
void fp_proto_task(void);

/* 初始化 */
void fp_proto_init(void);

/* 设置完整包回调 */
void fp_proto_set_callback(fp_packet_cb_t cb);

/* 查询是否有完整包待处理 */
BOOL fp_proto_packet_ready(void);

/* ===== 指令包组装辅助函数 ===== */

/* 计算校验和：从 data[0] 到 data[len-1] 求和取低16位 */
uint16_t fp_proto_checksum(const uint8_t *data, uint16_t len);

/* 组装指令包并返回总长度
 * buf: 输出缓冲(至少 param_len+12 字节)
 * cmd: 指令码
 * params: 参数数据(可NULL)
 * param_len: 参数字节数
 * 返回: 完整指令包总长度 */
uint16_t fp_proto_build_cmd(uint8_t *buf, uint8_t cmd, const uint8_t *params, uint8_t param_len);

#ifdef __cplusplus
}
#endif
#endif
