/********************************** (C) COPYRIGHT *******************************
 * File Name          : fp_app_cmds.c
 * Description        : 指纹模块 CLI 命令: fp_enroll / fp_verify / fp_delete /
 *                      fp_clear / fp_status / fp_cancel.
 *                      同时注册指纹状态机的通知回调,
 *                      将 fp_msg_t 消息格式化为 CLI 文本输出。
 *******************************************************************************/
#include "cli.h"

#include <stdlib.h>
#include <string.h>

#include "fp_sm.h"
#include "user_flash.h"
#include "keyboard_dispatch.h"

/* =======================================================================
 * 私有: 状态名称映射
 * ===================================================================== */
static const char *fp_state_name(fp_state_t s)
{
    switch (s) {
    case FP_IDLE:       return "IDLE";
    case FP_ENROLL:     return "ENROLL";
    case FP_VERIFY:     return "VERIFY";
    case FP_DELETE_ONE: return "DELETE";
    case FP_CLEAR_ALL:  return "CLEAR";
    case FP_CANCEL:     return "CANCEL";
    case FP_BLN_SET:    return "BLN_SET";
    case FP_READ_INDEX: return "READ_INDEX";
    default:            return "UNKNOWN";
    }
}

/* =======================================================================
 * 指纹通知回调: 将 fp_msg_t 消息格式化为 CLI 文本输出
 * ===================================================================== */
static void fp_msg_handler(fp_msg_t msg, uint16_t param1, uint16_t param2)
{
    switch (msg) {
    case FP_MSG_ENROLL_OK:
        cli_print("[FP] 注册成功, ID=%u\r\n", (unsigned)param1);
        break;

    case FP_MSG_ENROLL_STEP: {
        /* 根据步骤码输出中文提示 */
        const char *hint = "未知步骤";
        switch (param1) {
        case 0x01: hint = "请按手指";   break;
        case 0x02: hint = "生成特征";   break;
        case 0x03: hint = "请移开手指"; break;
        case 0x04: hint = "合并模板";   break;
        case 0x05: hint = "重复检测";   break;
        case 0x06: hint = "存储";       break;
        default: break;
        }
        cli_print("[FP] 步骤: step=%u, n=%u (%s)\r\n",
                  (unsigned)param1, (unsigned)param2, hint);
        break;
    }

    case FP_MSG_ENROLL_FAIL:
        cli_print("[FP] 注册失败, code=0x%02X\r\n", (unsigned)param1);
        break;

    case FP_MSG_VERIFY_OK:
        cli_print("[FP] 验证通过, ID=%u, 得分=%u\r\n",
                  (unsigned)param1, (unsigned)param2);
        /* 指纹 ID 对应用户 ID: 读取用户数据并通过键盘发送 */
        {
            static char    name_buf[USER_NAME_SIZE];
            static uint8_t data_buf[USER_DATA_SIZE];
            kbd_channel_t ch = KBD_CH_NONE;
            if (user_flash_get((uint8_t)param1, name_buf, data_buf, &ch)) {
                /* 通道无效(旧数据/未设置)或未选通道时回退当前默认通道 */
                if ((ch & KBD_CH_BOTH) != ch || ch == KBD_CH_NONE) {
                    ch = keyboard_get_channel();
                }
                cli_print("[FP] 用户 \"%s\" 数据已加载, 通道=%s, 开始发送\r\n",
                          name_buf, keyboard_channel_name(ch));
                keyboard_type_text((const char *)data_buf, ch);
            } else {
                cli_print("[FP] 用户 ID=%u 无数据\r\n", (unsigned)param1);
            }
        }
        break;

    case FP_MSG_VERIFY_FAIL:
        cli_print("[FP] 验证失败, code=0x%02X\r\n", (unsigned)param1);
        break;

    case FP_MSG_DELETE_OK:
        cli_print("[FP] 删除成功\r\n");
        break;

    case FP_MSG_DELETE_FAIL:
        cli_print("[FP] 删除失败, code=0x%02X\r\n", (unsigned)param1);
        break;

    case FP_MSG_CLEAR_OK:
        cli_print("[FP] 清空成功\r\n");
        break;

    case FP_MSG_CLEAR_FAIL:
        cli_print("[FP] 清空失败, code=0x%02X\r\n", (unsigned)param1);
        break;

    case FP_MSG_TIMEOUT:
        cli_print("[FP] 操作超时\r\n");
        break;

    case FP_MSG_CANCELLED:
        cli_print("[FP] 已取消\r\n");
        break;

    case FP_MSG_BUSY:
        cli_print("[FP] 忙, 请先完成当前操作\r\n");
        break;

    case FP_MSG_BLN_OK:
        cli_print("[FP] 呼吸灯已设为 %s模式\r\n",
                  param1 == 0xFF ? "自动" : "手动");
        break;

    case FP_MSG_BLN_FAIL:
        cli_print("[FP] 呼吸灯模式设置失败, code=0x%02X\r\n", (unsigned)param1);
        break;

    case FP_MSG_READ_INDEX_OK: {
        const uint8_t *idx = fp_sm_get_index_table();
        cli_print("[FP] 索引表读取成功, 共 %u 字节:\r\n", (unsigned)FP_INDEX_TABLE_SIZE);
        for (int i = 0; i < FP_INDEX_TABLE_SIZE; i++) {
            cli_print("%02X ", (unsigned)idx[i]);
            if ((i + 1) % 16 == 0) {
                cli_print("\r\n");
            }
        }
        break;
    }

    case FP_MSG_READ_INDEX_FAIL:
        cli_print("[FP] 索引表读取失败, code=0x%02X, page=%u\r\n",
                  (unsigned)param1, (unsigned)param2);
        break;

    default:
        cli_print("[FP] 未知消息 0x%02X p1=%u p2=%u\r\n",
                  (unsigned)msg, (unsigned)param1, (unsigned)param2);
        break;
    }
}

/* =======================================================================
 * 独立命令实现
 * ===================================================================== */
static int cmd_fp_enroll(int argc, char *argv[])
{
    if (argc < 2) {
        cli_print("  usage: fp_enroll <id>\r\n");
        return -1;
    }
    uint16_t id = (uint16_t)atoi(argv[1]);
    if (fp_sm_enroll(id)) {
        cli_print("  fp_enroll started, ID=%u\r\n", (unsigned)id);
    } else {
        cli_print("  fp busy, cannot start enroll\r\n");
    }
    return 0;
}

CLI_CMD_REGISTER("fp_enroll", cmd_fp_enroll, "fp_enroll <id>        enroll fingerprint to given ID");

static int cmd_fp_verify(int argc, char *argv[])
{
    if (fp_sm_verify()) {
        cli_print("  fp_verify started\r\n");
    } else {
        cli_print("  fp busy, cannot start verify\r\n");
    }
    return 0;
}

CLI_CMD_REGISTER("fp_verify", cmd_fp_verify, "fp_verify             1:N verify");

static int cmd_fp_delete(int argc, char *argv[])
{
    if (argc < 2) {
        cli_print("  usage: fp_delete <id>\r\n");
        return -1;
    }
    uint16_t id = (uint16_t)atoi(argv[1]);
    if (fp_sm_delete(id)) {
        cli_print("  fp_delete started, ID=%u\r\n", (unsigned)id);
    } else {
        cli_print("  fp busy, cannot start delete\r\n");
    }
    return 0;
}

CLI_CMD_REGISTER("fp_delete", cmd_fp_delete, "fp_delete <id>        delete fingerprint of given ID");

static int cmd_fp_clear(int argc, char *argv[])
{
    if (fp_sm_clear()) {
        cli_print("  fp_clear started\r\n");
    } else {
        cli_print("  fp busy, cannot start clear\r\n");
    }
    return 0;
}

CLI_CMD_REGISTER("fp_clear", cmd_fp_clear, "fp_clear              clear all fingerprints");

static int cmd_fp_status(int argc, char *argv[])
{
    cli_print("  fp state: %s\r\n", fp_state_name(fp_sm_get_state()));
    return 0;
}

CLI_CMD_REGISTER("fp_status", cmd_fp_status, "fp_status             query current state");

static int cmd_fp_cancel(int argc, char *argv[])
{
    if (fp_sm_cancel()) {
        cli_print("  fp_cancel sent\r\n");
    } else {
        cli_print("  fp idle, nothing to cancel\r\n");
    }
    return 0;
}

CLI_CMD_REGISTER("fp_cancel", cmd_fp_cancel, "fp_cancel             cancel current operation");

static int cmd_fp_bln(int argc, char *argv[])
{
    if (argc < 2) {
        cli_print("  usage: fp_bln <auto|manual>\r\n"
                  "         auto   = 模组自动呼吸(默认, 上电自动模式)\r\n"
                  "         manual = 手动模式(掉电保存, 由 PS_ControlBLN 控制)\r\n");
        return 0;
    }
    BOOL auto_mode;
    if (!strcmp(argv[1], "auto")   || !strcmp(argv[1], "a") || !strcmp(argv[1], "1")) {
        auto_mode = TRUE;
    } else if (!strcmp(argv[1], "manual") || !strcmp(argv[1], "m") || !strcmp(argv[1], "0")) {
        auto_mode = FALSE;
    } else {
        cli_print("  invalid mode '%s' (auto|manual)\r\n", argv[1]);
        return -1;
    }
    if (fp_sm_set_bln_mode(auto_mode)) {
        cli_print("  fp_bln: switching to %s mode...\r\n", auto_mode ? "auto" : "manual");
    } else {
        cli_print("  fp busy, cannot set bln mode now\r\n");
        return -1;
    }
    return 0;
}

CLI_CMD_REGISTER("fp_bln", cmd_fp_bln, "fp_bln <auto|manual>     set fingerprint breathing-light mode");

static int cmd_fp_read_index(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    if (fp_sm_read_index()) {
        cli_print("  fp_read_index started\r\n");
    } else {
        cli_print("  fp busy, cannot start read index\r\n");
    }
    return 0;
}

CLI_CMD_REGISTER("fp_read_index", cmd_fp_read_index, "fp_read_index          read all fingerprint index table");

/* =======================================================================
 * 初始化入口
 * ===================================================================== */

void fp_app_cmds_init(void)
{
    /* 仅注册指纹通知回调 (命令本身已由链接器段静态注册) */
    fp_sm_set_notify_cb(fp_msg_handler);
}
