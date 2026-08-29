/********************************** (C) COPYRIGHT *******************************
 * File Name          : user_flash_cli.c
 * Description        : 用户数据存储 CLI 命令层: user set/get/del/clear/list/count/help
 *                      仿 fp_app_cmds.c 的 cli_cmd_t 表 + 子命令分发模式
 *******************************************************************************/
#include "cli.h"

#include <stdlib.h>
#include <string.h>

#include "user_flash.h"
#include "keyboard_dispatch.h"

/* =======================================================================
 * 参数解析 (十进制/十六进制)
 * ===================================================================== */
static long parse_num(const char *s)
{
    if (!s) return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return strtol(s, NULL, 16);
    return strtol(s, NULL, 10);
}

/* 校验 id 在 0~USER_FLASH_MAX_USERS-1 范围内, 失败打印提示并返回 FALSE */
static BOOL parse_id(const char *s, uint8_t *id_out)
{
    long id = parse_num(s);
    if (id < 0 || id >= USER_FLASH_MAX_USERS) {
        cli_print("  invalid id '%s', range 0~%d\r\n",
                  s ? s : "", (int)(USER_FLASH_MAX_USERS - 1));
        return FALSE;
    }
    *id_out = (uint8_t)id;
    return TRUE;
}

/* 解析上报通道: 支持 none/ble/usb/both 或数字 0~3 */
static BOOL parse_channel(const char *s, kbd_channel_t *ch_out)
{
    if (!s) return FALSE;
    if (!strcmp(s, "none")) { *ch_out = KBD_CH_NONE; return TRUE; }
    if (!strcmp(s, "ble"))  { *ch_out = KBD_CH_BLE;  return TRUE; }
    if (!strcmp(s, "usb"))  { *ch_out = KBD_CH_USB;  return TRUE; }
    if (!strcmp(s, "both")) { *ch_out = KBD_CH_BOTH; return TRUE; }
    long v = parse_num(s);
    if (v >= 0 && v <= 3) { *ch_out = (kbd_channel_t)v; return TRUE; }
    return FALSE;
}

/* =======================================================================
 * user 命令实现
 * ===================================================================== */
static int cmd_user(int argc, char *argv[])
{
    /* 无参数或 help 子命令: 输出子命令列表 */
    if (argc < 2 || !strcmp(argv[1], "help")) {
        cli_print("  user subcommands:\r\n");
        cli_print("    user set <id> <name> <ch> <data...>  设置用户 (name<=15, data<=110)\r\n");
        cli_print("      ch: none|ble|usb|both (或 0~3)\r\n");
        cli_print("    user get <id>                    读取用户\r\n");
        cli_print("    user del <id>                    删除指定用户\r\n");
        cli_print("    user clear                       清空全部用户\r\n");
        cli_print("    user list                        列出所有非空用户\r\n");
        cli_print("    user count                       统计非空用户数\r\n");
        cli_print("    user help                        显示此帮助\r\n");
        return 0;
    }

    /* ---- set <id> <name> <ch> <data...> ---- */
    if (!strcmp(argv[1], "set")) {
        if (argc < 6) {
            cli_print("  usage: user set <id> <name> <ch> <data...>\r\n"
                      "         ch = none|ble|usb|both (或 0~3)\r\n");
            return -1;
        }
        uint8_t id;
        if (!parse_id(argv[2], &id)) return -1;

        kbd_channel_t ch;
        if (!parse_channel(argv[4], &ch)) {
            cli_print("  invalid channel '%s' (none|ble|usb|both)\r\n", argv[4]);
            return -1;
        }

        /* 拼接 argv[5..argc-1] 为 data 字符串 (空格分隔) */
        static char data_buf[USER_DATA_SIZE];
        int n = 0;
        for (int i = 5; i < argc && n < (int)(sizeof(data_buf) - 1); i++) {
            if (i > 5 && n < (int)(sizeof(data_buf) - 1)) {
                data_buf[n++] = ' ';
            }
            for (const char *p = argv[i]; *p && n < (int)(sizeof(data_buf) - 1); p++) {
                data_buf[n++] = *p;
            }
        }
        data_buf[n] = '\0';

        if (user_flash_set(id, argv[3], (const uint8_t *)data_buf, ch)) {
            cli_print("  user set ok, id=%u name=\"%s\" ch=%s data=\"%s\"\r\n",
                      (unsigned)id, argv[3], keyboard_channel_name(ch), data_buf);
        } else {
            cli_print("  user set FAILED (id=%u)\r\n", (unsigned)id);
            return -1;
        }
        return 0;
    }

    /* ---- get <id> ---- */
    if (!strcmp(argv[1], "get")) {
        if (argc < 3) {
            cli_print("  usage: user get <id>\r\n");
            return -1;
        }
        uint8_t id;
        if (!parse_id(argv[2], &id)) return -1;

        static char name_buf[USER_NAME_SIZE];
        static uint8_t data_buf[USER_DATA_SIZE];
        kbd_channel_t ch = KBD_CH_NONE;
        if (user_flash_get(id, name_buf, data_buf, &ch)) {
            cli_print("  id=%u name=\"%s\" ch=%s data=\"%s\"\r\n",
                      (unsigned)id, name_buf, keyboard_channel_name(ch), (char *)data_buf);
        } else {
            cli_print("  id=%u empty\r\n", (unsigned)id);
        }
        return 0;
    }

    /* ---- del <id> ---- */
    if (!strcmp(argv[1], "del")) {
        if (argc < 3) {
            cli_print("  usage: user del <id>\r\n");
            return -1;
        }
        uint8_t id;
        if (!parse_id(argv[2], &id)) return -1;

        if (user_flash_delete(id)) {
            cli_print("  user del ok, id=%u\r\n", (unsigned)id);
        } else {
            cli_print("  user del FAILED (id=%u)\r\n", (unsigned)id);
            return -1;
        }
        return 0;
    }

    /* ---- clear ---- */
    if (!strcmp(argv[1], "clear")) {
        if (user_flash_clear_all()) {
            cli_print("  user clear ok (all %d users erased)\r\n",
                      (int)USER_FLASH_MAX_USERS);
        } else {
            cli_print("  user clear FAILED\r\n");
            return -1;
        }
        return 0;
    }

    /* ---- list ---- */
    if (!strcmp(argv[1], "list")) {
        uint8_t n = 0;
        static char name_buf[USER_NAME_SIZE];
        static uint8_t data_buf[USER_DATA_SIZE];
        kbd_channel_t ch = KBD_CH_NONE;
        /* 星号串: 按用户数据实际长度遮蔽打印 (长度对齐 USER_DATA_SIZE=111) */
        static const char STARS[] = "*************************************************************************************************************"; /* 111 */
        cli_print("  %-4s %-16s %-5s %s\r\n", "id", "name", "ch", "data");
        for (uint8_t id = 0; id < USER_FLASH_MAX_USERS; id++) {
            if (user_flash_get(id, name_buf, data_buf, &ch)) {
                /* 打印与用户数据等长的 '*' 遮蔽敏感内容 */
                cli_print("  %-4u %-16s %-5s %.*s\r\n",
                          (unsigned)id, name_buf, keyboard_channel_name(ch),
                          (int)strlen((const char *)data_buf), STARS);
                /* 需要查看原始数据时, 打开下面一行即可 */
                /* cli_print("  %-4u %-16s %-5s %s\r\n",
                          (unsigned)id, name_buf, keyboard_channel_name(ch), (char *)data_buf); */
                n++;
            }
        }
        cli_print("  total: %u user(s)\r\n", (unsigned)n);
        return 0;
    }

    /* ---- count ---- */
    if (!strcmp(argv[1], "count")) {
        cli_print("  users: %u / %d\r\n",
                  (unsigned)user_flash_count(), (int)USER_FLASH_MAX_USERS);
        return 0;
    }

    cli_print("  unknown subcommand '%s'\r\n  usage: user <set|get|del|clear|list|count|help>\r\n",
              argv[1]);
    return -1;
}

/* =======================================================================
 * 静态命令注册 (链接器 "cli_cmds" 段), 无需运行时初始化
 * ===================================================================== */

CLI_CMD_REGISTER("user", cmd_user, "User data flash storage: set/get/del/clear/list/count");

