/********************************** (C) COPYRIGHT *******************************
 * File Name          : fp_app_cmds.c
 * Description        : 指纹模块 CLI 命令: enroll / verify / delete / clear /
 *                      status / cancel / help. 同时注册指纹状态机的通知回调,
 *                      将 fp_msg_t 消息格式化为 CLI 文本输出。
 *******************************************************************************/
#include "cli.h"

#include <stdlib.h>
#include <string.h>

#include "fp_sm.h"

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

    default:
        cli_print("[FP] 未知消息 0x%02X p1=%u p2=%u\r\n",
                  (unsigned)msg, (unsigned)param1, (unsigned)param2);
        break;
    }
}

/* =======================================================================
 * fp 命令实现
 * ===================================================================== */
static int cmd_fp(int argc, char *argv[])
{
    /* 无参数或 help 子命令: 输出子命令列表 */
    if (argc < 2 || !strcmp(argv[1], "help")) {
        cli_print("  fp subcommands:\r\n");
        cli_print("    fp enroll <id>   注册指纹到指定 ID\r\n");
        cli_print("    fp verify        1:N 验证\r\n");
        cli_print("    fp delete <id>   删除指定 ID\r\n");
        cli_print("    fp clear         清空全部指纹\r\n");
        cli_print("    fp status        查询当前状态\r\n");
        cli_print("    fp cancel        取消当前操作\r\n");
        cli_print("    fp help          显示此帮助\r\n");
        return 0;
    }

    if (!strcmp(argv[1], "enroll")) {
        if (argc < 3) {
            cli_print("  usage: fp enroll <id>\r\n");
            return -1;
        }
        uint16_t id = (uint16_t)atoi(argv[2]);
        if (fp_sm_enroll(id)) {
            cli_print("  fp enroll started, ID=%u\r\n", (unsigned)id);
        } else {
            cli_print("  fp busy, cannot start enroll\r\n");
        }
        return 0;
    }

    if (!strcmp(argv[1], "verify")) {
        if (fp_sm_verify()) {
            cli_print("  fp verify started\r\n");
        } else {
            cli_print("  fp busy, cannot start verify\r\n");
        }
        return 0;
    }

    if (!strcmp(argv[1], "delete")) {
        if (argc < 3) {
            cli_print("  usage: fp delete <id>\r\n");
            return -1;
        }
        uint16_t id = (uint16_t)atoi(argv[2]);
        if (fp_sm_delete(id)) {
            cli_print("  fp delete started, ID=%u\r\n", (unsigned)id);
        } else {
            cli_print("  fp busy, cannot start delete\r\n");
        }
        return 0;
    }

    if (!strcmp(argv[1], "clear")) {
        if (fp_sm_clear()) {
            cli_print("  fp clear started\r\n");
        } else {
            cli_print("  fp busy, cannot start clear\r\n");
        }
        return 0;
    }

    if (!strcmp(argv[1], "status")) {
        cli_print("  fp state: %s\r\n", fp_state_name(fp_sm_get_state()));
        return 0;
    }

    if (!strcmp(argv[1], "cancel")) {
        if (fp_sm_cancel()) {
            cli_print("  fp cancel sent\r\n");
        } else {
            cli_print("  fp busy, cannot cancel\r\n");
        }
        return 0;
    }

    cli_print("  unknown subcommand '%s'\r\n  usage: fp <enroll|verify|delete|clear|status|cancel|help>\r\n",
              argv[1]);
    return -1;
}

/* =======================================================================
 * 静态命令注册 (链接器 "cli_cmds" 段) + 初始化入口
 * fp 命令无需运行时注册, 由 cli core 通过段符号自动发现
 * ===================================================================== */

CLI_CMD_REGISTER("fp", cmd_fp, "Fingerprint control: enroll/verify/delete/clear/status/cancel");

void fp_app_cmds_init(void)
{
    /* 仅注册指纹通知回调 (fp 命令本身已由链接器段静态注册) */
    fp_sm_set_notify_cb(fp_msg_handler);
}

