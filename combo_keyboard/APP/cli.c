#include "cli.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#if CLI_ENABLE_LINKER_CMD_REGISTRATION
/* ======================== 1. 链接器符号声明 ======================== */
/* 这些符号由链接器脚本生成，代表 "cli_cmds" 段的起始和结束地址 */
#if defined(__GNUC__)
extern const cli_cmd_entry_t __start_cli_cmds;
extern const cli_cmd_entry_t __stop_cli_cmds;
#endif
#endif

/* ======================== 2. 内部状态变量 ======================== */
static char     s_line[CLI_LINE_MAX];
static uint16_t s_line_len    = 0;
static uint16_t s_cursor_pos  = 0;
static uint8_t  s_line_ready  = 0;

static char     s_history_buf[8][CLI_LINE_MAX];
static uint8_t  s_history_count = 0;
static int8_t   s_history_idx   = -1;

static uint8_t  s_esc_state = 0;

static cli_cmd_t s_cmds[CLI_CMD_MAX];
static uint8_t   s_cmd_count = 0;

/* ======================== 3. HAL 弱函数 ======================== */
__attribute__((weak)) void cli_tx_raw(const char *data, uint16_t len)
{
    /* 弱实现：默认使用标准输出，实际项目中应替换为 UART 发送 */
    for (uint16_t i = 0; i < len; i++) {
        putchar(data[i]);
    }
    fflush(stdout);
}

/* ======================== 4. 内部辅助函数 ======================== */

static void redraw_line(void)
{
    const char *prompt = CLI_PROMPT;
    uint16_t prompt_len = strlen(prompt);

    /* 1. 回到行首 */
    cli_tx_raw("\r", 1);
    /* 2. ⭐ 清除整行（从光标到行尾），彻底消除残留字符 */
    cli_tx_raw("\033[K", 4);
    /* 3. 打印提示符 */
    cli_tx_raw(prompt, prompt_len);
    /* 4. 打印当前行内容 */
    if (s_line_len > 0) {
        cli_tx_raw(s_line, s_line_len);
    }
    /* 5. 重新定位光标到正确位置 */
    if (s_cursor_pos < s_line_len) {
        /* 如果光标不在行尾，需要回退 (s_line_len - s_cursor_pos) 格 */
        uint16_t back = s_line_len - s_cursor_pos;
        char esc_buf[8];
        int esc_len = snprintf(esc_buf, sizeof(esc_buf), "\033[%uD", back);
        if (esc_len > 0) {
            cli_tx_raw(esc_buf, (uint16_t)esc_len);
        }
    }
}

/**
 * @brief  历史记录回溯
 * @param  dir: -1 = 上翻(更旧), +1 = 下翻(更新)
 */
static void cli_history_recall(int8_t dir)
{
    if (s_history_count == 0) return;

    int8_t new_idx = s_history_idx;

    if (dir < 0) {
        /* ⭐ 上翻：向更旧的命令移动 */
        if (new_idx == -1) {
            /* 从当前新行首次上翻，直接跳到最新的历史记录 */
            new_idx = (int8_t)(s_history_count - 1);
        } else if (new_idx > 0) {
            new_idx--;
        }
        /* new_idx == 0 时不再递减，停留在最旧的命令 */
    } else {
        /* ⭐ 下翻：向更新的命令移动 */
        if (new_idx == -1) {
            /* 已经在最新位置继续下翻，保持在新行状态 */
            /* do nothing */
        } else {
            new_idx++;
            if (new_idx >= (int8_t)s_history_count) {
                /* 越过最新记录，回到当前新行输入状态 */
                new_idx = -1;
            }
        }
    }

    if (new_idx != s_history_idx) {
        s_history_idx = new_idx;

        if (s_history_idx == -1) {
            /* 恢复到空行状态 */
            s_line[0] = '\0';
            s_line_len = 0;
            s_cursor_pos = 0;
        } else {
            /* 加载历史命令 */
            strncpy(s_line, s_history_buf[s_history_idx], CLI_LINE_MAX - 1);
            s_line[CLI_LINE_MAX - 1] = '\0';
            s_line_len = strlen(s_line);
            s_cursor_pos = s_line_len;
        }
        redraw_line();
    }
}

/**
 * @brief  在所有注册源（动态数组 + 静态段）中查找命令
 */
static cli_cmd_fn_t find_cmd_handler(const char *name, const char **out_help)
{
    /* 1. 优先查找动态注册的命令 */
    for (uint8_t i = 0; i < s_cmd_count; i++) {
        if (strcmp(s_cmds[i].name, name) == 0) {
            if (out_help) *out_help = s_cmds[i].help;
            return s_cmds[i].handler;
        }
    }

#if CLI_ENABLE_LINKER_CMD_REGISTRATION
#if defined(__GNUC__)
    /* 2. 遍历静态链接器段中的命令 */
    const cli_cmd_entry_t *p = &__start_cli_cmds;
    const cli_cmd_entry_t *end = &__stop_cli_cmds;
    while (p < end) {
        if (strcmp(p->name, name) == 0) {
            if (out_help) *out_help = p->help;
            return p->handler;
        }
        p++;
    }
#endif
#endif

    return NULL;
}

static void cli_tab_complete(void)
{
    /* 简单的 Tab 补全：如果当前输入是某个命令的前缀，且唯一匹配，则补全 */
    if (s_line_len == 0) return;

    const char *match = NULL;
    int match_count = 0;

    /* 检查动态命令 */
    for (uint8_t i = 0; i < s_cmd_count; i++) {
        if (strncmp(s_cmds[i].name, s_line, s_line_len) == 0) {
            match = s_cmds[i].name;
            match_count++;
        }
    }

#if CLI_ENABLE_LINKER_CMD_REGISTRATION
#if defined(__GNUC__)
    /* 检查静态命令 */
    const cli_cmd_entry_t *p = &__start_cli_cmds;
    const cli_cmd_entry_t *end = &__stop_cli_cmds;
    while (p < end) {
        if (strncmp(p->name, s_line, s_line_len) == 0) {
            match = p->name;
            match_count++;
        }
        p++;
    }
#endif
#endif

    if (match_count == 1 && match) {
        /* 唯一匹配，执行补全 */
        uint16_t add_len = strlen(match) - s_line_len;
        if (s_line_len + add_len < CLI_LINE_MAX - 1) {
            strcpy(s_line + s_line_len, match + s_line_len);
            s_line_len += add_len;
            s_cursor_pos = s_line_len;
            /* 打印补全的部分 */
            cli_tx_raw(match + s_line_len - add_len, add_len);
        }
    } else if (match_count > 1) {
        /* 多个匹配，打印所有选项 */
        cli_tx_raw("\r\n", 2);

        for (uint8_t i = 0; i < s_cmd_count; i++) {
            if (strncmp(s_cmds[i].name, s_line, s_line_len) == 0) {
                cli_print("  %s\r\n", s_cmds[i].name);
            }
        }

#if CLI_ENABLE_LINKER_CMD_REGISTRATION
#if defined(__GNUC__)
        const cli_cmd_entry_t *p2 = &__start_cli_cmds;
        const cli_cmd_entry_t *end2 = &__stop_cli_cmds;
        while (p2 < end2) {
            if (strncmp(p2->name, s_line, s_line_len) == 0) {
                cli_print("  %s\r\n", p2->name);
            }
            p2++;
        }
#endif
#endif

        redraw_line();
    }
}

/* ======================== 5. 内置 help 命令 ======================== */

static int cmd_help_builtin(int argc, char *argv[])
{
    (void)argc; (void)argv;
    cli_print("Available commands:\r\n");

    /* 打印动态命令 */
    for (uint8_t i = 0; i < s_cmd_count; i++) {
        cli_print("  %-12s %s\r\n", s_cmds[i].name, s_cmds[i].help ? s_cmds[i].help : "");
    }

#if CLI_ENABLE_LINKER_CMD_REGISTRATION
#if defined(__GNUC__)
    /* 打印静态命令 */
    const cli_cmd_entry_t *p = &__start_cli_cmds;
    const cli_cmd_entry_t *end = &__stop_cli_cmds;
    while (p < end) {
        cli_print("  %-12s %s\r\n", p->name, p->help ? p->help : "");
        p++;
    }
#endif
#endif

    return 0;
}

/* ======================== 6. 公共 API 实现 ======================== */

void cli_init(void)
{
    s_line[0]    = '\0';
    s_line_len   = 0;
    s_cursor_pos = 0;
    s_line_ready = 0;
    s_esc_state  = 0;
    s_cmd_count  = 0;
    s_history_count = 0;
    s_history_idx   = -1;
    memset(s_cmds, 0, sizeof(s_cmds));
    memset(s_history_buf, 0, sizeof(s_history_buf));

    /* 自动注册内置 help 命令 (动态方式) */
    cli_register_cmd("help", cmd_help_builtin, "List all available commands");
}

int cli_register_cmd(const char *name, cli_cmd_fn_t fn, const char *help)
{
    if (s_cmd_count >= CLI_CMD_MAX) return -1;
    s_cmds[s_cmd_count].name = name;
    s_cmds[s_cmd_count].handler = fn;
    s_cmds[s_cmd_count].help = help;
    s_cmd_count++;
    return 0;
}

int cli_register_cmds(const cli_cmd_t *table)
{
    int count = 0;
    while (table && table[count].name) {
        if (cli_register_cmd(table[count].name, table[count].handler, table[count].help) < 0) {
            return count;
        }
        count++;
    }
    return count;
}

void cli_print_prompt(void)
{
    cli_tx_raw(CLI_PROMPT, strlen(CLI_PROMPT));
}

void cli_print(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        cli_tx_raw(buf, (uint16_t)len);
    }
}

int cli_rx_byte(uint8_t b)
{
    /* ESC 序列状态机处理 */
    if (s_esc_state == 1) {
        if (b == '[') {
            s_esc_state = 2;
            return 0;
        }
        s_esc_state = 0;
    } else if (s_esc_state == 2) {
        s_esc_state = 0;
        switch (b) {
            case 'A': cli_history_recall(-1); break; /* ↑ 上箭头 → 更旧 */
            case 'B': cli_history_recall(1);  break; /* ↓ 下箭头 → 更新 */
            case 'C': /* → 右箭头 (可选实现) */ break;
            case 'D': /* ← 左箭头 (可选实现) */ break;
            default: break;
        }
        return 0;
    }

    switch (b) {
        case 0x1B: /* ESC */
            s_esc_state = 1;
            break;

        case '\r':
        case '\n':
            cli_tx_raw("\r\n", 2);
            s_line[s_line_len] = '\0';
            s_line_ready = 1;

            /* ⭐ 历史记录去重与保存 */
            if (s_line_len > 0) {
                uint8_t should_save = 1;

                /* 如果历史不为空，且当前输入与最新一条历史记录完全相同，则跳过保存 */
                if (s_history_count > 0 &&
                    strcmp(s_line, s_history_buf[s_history_count - 1]) == 0) {
                    should_save = 0;
                }

                if (should_save) {
                    if (s_history_count < 8) {
                        strcpy(s_history_buf[s_history_count++], s_line);
                    } else {
                        memmove(s_history_buf[0], s_history_buf[1], 7 * CLI_LINE_MAX);
                        strcpy(s_history_buf[7], s_line);
                    }
                }
            }
            s_history_idx = -1;
            break;

        case 0x7F: /* Backspace (DEL) */
        case 0x08: /* Backspace (BS) */
            if (s_cursor_pos > 0) {
                memmove(&s_line[s_cursor_pos - 1], &s_line[s_cursor_pos], s_line_len - s_cursor_pos);
                s_cursor_pos--;
                s_line_len--;
                s_line[s_line_len] = '\0';
                redraw_line();
            }
            break;

        case '\t': /* Tab 补全 */
            cli_tab_complete();
            break;

        default:
            /* 可打印字符 */
            if (b >= 0x20 && b < 0x7F && s_line_len < CLI_LINE_MAX - 1) {
                memmove(&s_line[s_cursor_pos + 1], &s_line[s_cursor_pos], s_line_len - s_cursor_pos);
                s_line[s_cursor_pos] = (char)b;
                s_cursor_pos++;
                s_line_len++;
                s_line[s_line_len] = '\0';

                /* 简单回显 */
                if (s_cursor_pos == s_line_len) {
                    cli_tx_raw((char*)&b, 1);
                } else {
                    redraw_line();
                }
            }
            break;
    }
    return 0;
}

int cli_task(void)
{
    if (!s_line_ready) return 0;
    s_line_ready = 0;

    char *argv[CLI_ARGV_MAX];
    int argc = 0;

    char *token = strtok(s_line, " \t");
    while (token && argc < CLI_ARGV_MAX) {
        argv[argc++] = token;
        token = strtok(NULL, " \t");
    }

    if (argc == 0) {
        redraw_line();
        return 0;
    }

    /* 使用统一的查找函数 */
    cli_cmd_fn_t handler = find_cmd_handler(argv[0], NULL);

    if (handler) {
        handler(argc, argv);
    } else {
        cli_print("  Unknown command: %s. Type 'help' to list all.\r\n", argv[0]);
    }

    s_line[0] = '\0';
    s_line_len = 0;
    s_cursor_pos = 0;

    cli_print_prompt();
    return handler ? 1 : 0;
}

void cli_print_help(void)
{
    cmd_help_builtin(0, NULL);
}
