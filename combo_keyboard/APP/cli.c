/********************************** (C) COPYRIGHT *******************************
 * File Name          : cli.c
 * Description        : Enhanced Lightweight CLI core with Tab completion
 *                      and command history.
 *******************************************************************************/
#include "cli.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* RISC-V PFIC interrupt control */
#ifndef __disable_irq
#define __disable_irq()  __asm volatile("csrci mstatus, 0x8")
#define __enable_irq()   __asm volatile("csrsi mstatus, 0x8")
#endif

/* --------------------------- Configuration & State ------------------------- */

// Command and history limits
#ifndef CLI_HISTORY_MAX
#define CLI_HISTORY_MAX     8
#endif

static cli_cmd_t  s_cmds[CLI_CMD_MAX];
static uint8_t    s_cmd_count;

/* Input line state */
static char       s_line[CLI_LINE_MAX];
static uint16_t   s_line_len;
static uint16_t   s_cursor_pos;            // Current cursor position for line editing

/* History buffer (circular buffer) */
static char       s_history_buf[CLI_HISTORY_MAX][CLI_LINE_MAX];
static int8_t     s_history_idx = -1;      // Index of the currently displayed history command
static uint8_t    s_history_count = 0;     // Total number of commands in history

/* Input state machine for handling escape sequences (arrow keys) */
typedef enum {
    CLI_INPUT_NORMAL,
    CLI_INPUT_WAIT_SPEC_KEY, // Received ESC (0x1b)
    CLI_INPUT_WAIT_FUNC_KEY, // Received ESC [ (0x5b)
} cli_input_state_t;

static cli_input_state_t s_input_state = CLI_INPUT_NORMAL;

/* Output and RX state */
static char       s_txbuf[CLI_LINE_MAX];
static char       s_ready_line[CLI_LINE_MAX];
static volatile uint8_t s_ready;
static uint8_t    s_last_was_eol;

/* ------------------------- weak HAL defaults ----------------------------- */

__attribute__((weak))
BOOL cli_uart_ready(void) { return TRUE; }

__attribute__((weak))
void cli_uart_send(const uint8_t *buf, uint16_t len)
{
    while (len--) cli_uart_putc(*buf++);
}

/* --------------------------- helpers ------------------------------------- */

static void tx_raw(const char *buf, uint16_t len)
{
    if (!cli_uart_ready()) return;
    cli_uart_send((const uint8_t *)buf, len);
}

static void tx_str(const char *s)
{
    tx_raw(s, (uint16_t)strlen(s));
}

/* Moves cursor back one position, prints space, moves cursor back again. */
static void echo_backspace(void)
{
    tx_raw("\b \b", 3);
}

/* Clears the entire line and moves cursor to the beginning. */
static void clear_line_and_home(void)
{
    tx_raw("\r\033[K", 4); // \r: Carriage Return, \033[K: Erase to End of Line
}

/* --------------------------- history & completion ------------------------ */

static void cli_history_add(const char *cmd)
{
    if (strlen(cmd) == 0) return;

    // Shift history down to make room for the new command at the top
    if (s_history_count == CLI_HISTORY_MAX) {
        // Buffer is full, shift all commands down
        for (int i = CLI_HISTORY_MAX - 1; i > 0; i--) {
            strcpy(s_history_buf[i], s_history_buf[i - 1]);
        }
    } else {
        // Buffer not full, shift existing commands down
        for (int i = s_history_count; i > 0; i--) {
            strcpy(s_history_buf[i], s_history_buf[i - 1]);
        }
        s_history_count++;
    }
    strcpy(s_history_buf[0], cmd);
    s_history_idx = -1; // Reset history index when a new command is entered
}

static void cli_history_recall(int8_t dir)
{
    if (s_history_count == 0) return;

    int8_t new_idx = s_history_idx + dir;

    // Check bounds
    if (new_idx < -1) new_idx = -1; // -1 means show current (empty) line
    if (new_idx >= s_history_count) new_idx = s_history_count - 1;

    if (new_idx != s_history_idx) {
        s_history_idx = new_idx;
        clear_line_and_home();
        cli_print_prompt();

        if (s_history_idx == -1) {
            // Show empty line
            s_line_len = 0;
            s_cursor_pos = 0;
            s_line[0] = '\0';
        } else {
            // Show historical command
            strcpy(s_line, s_history_buf[s_history_idx]);
            s_line_len = strlen(s_line);
            s_cursor_pos = s_line_len;
            tx_str(s_line);
        }
    }
}

static void cli_tab_complete(void)
{
    int matches = 0;
    const char *match_cmd = NULL;

    // Find the last word in the buffer to use as the prefix for completion
    char *prefix_start = s_line;
    for (int i = s_cursor_pos - 1; i >= 0; i--) {
        if (s_line[i] == ' ' || s_line[i] == '\t') {
            prefix_start = &s_line[i + 1];
            break;
        }
    }
    uint16_t prefix_len = s_cursor_pos - (prefix_start - s_line);

    // 1. Find matches
    for (uint8_t i = 0; i < s_cmd_count; i++) {
        if (strncmp(prefix_start, s_cmds[i].name, prefix_len) == 0) {
            matches++;
            match_cmd = s_cmds[i].name;
        }
    }

    if (matches == 1) {
        // 2. Single match: complete the word
        const char *suffix = match_cmd + prefix_len;
        uint16_t suffix_len = strlen(suffix);

        // Make space for the new characters
        memmove(s_line + s_cursor_pos + suffix_len, s_line + s_cursor_pos, s_line_len - s_cursor_pos + 1);
        // Insert the suffix
        memcpy(s_line + s_cursor_pos, suffix, suffix_len);

        s_line_len += suffix_len;
        s_cursor_pos += suffix_len;

        // Echo the completed part
        tx_raw(suffix, suffix_len);
    } else if (matches > 1) {
        // 3. Multiple matches: list them
        tx_raw("\r\n", 2);
        for (uint8_t i = 0; i < s_cmd_count; i++) {
            if (strncmp(prefix_start, s_cmds[i].name, prefix_len) == 0) {
                tx_str(s_cmds[i].name);
                tx_raw("  ", 2);
            }
        }
        // Redraw prompt and current line
        tx_raw("\r\n", 2);
        cli_print_prompt();
        tx_raw(s_line, s_line_len);
        // Move cursor back to its original position
        for (uint16_t i = 0; i < s_line_len - s_cursor_pos; i++) {
            tx_raw("\b", 1);
        }
    }
}

/* --------------------------- public API ---------------------------------- */

void cli_init(void)
{
    s_cmd_count = 0;
    s_line_len  = 0;
    s_cursor_pos = 0;
    s_line[0]   = 0;
    s_ready     = 0;
    s_history_idx = -1;
    s_history_count = 0;
    memset(s_cmds, 0, sizeof(s_cmds));
}

int cli_register_cmd(const char *name, cli_cmd_fn_t fn, const char *help)
{
    if (!name || !fn) return -1;
    if (s_cmd_count >= CLI_CMD_MAX) return -1;
    s_cmds[s_cmd_count].name    = name;
    s_cmds[s_cmd_count].handler = fn;
    s_cmds[s_cmd_count].help    = help ? help : "";
    s_cmd_count++;
    return 0;
}

int cli_register_cmds(const cli_cmd_t *table)
{
    int added = 0;
    if (!table) return -1;
    for (; table->name && table->handler; table++) {
        if (cli_register_cmd(table->name, table->handler, table->help) == 0) {
            added++;
        } else {
            return -1;
        }
    }
    return added;
}

int cli_rx_byte(uint8_t b)
{
    char c = (char)b;

    // --- State Machine for Special Keys ---
    if (s_input_state != CLI_INPUT_NORMAL) {
        if (s_input_state == CLI_INPUT_WAIT_SPEC_KEY) {
            if (c == 0x5b) { // '['
                s_input_state = CLI_INPUT_WAIT_FUNC_KEY;
            } else {
                s_input_state = CLI_INPUT_NORMAL;
            }
            return 0;
        } else if (s_input_state == CLI_INPUT_WAIT_FUNC_KEY) {
            s_input_state = CLI_INPUT_NORMAL;

            if (c == 0x41) { // Up Arrow
                cli_history_recall(1);
                return 0;
            } else if (c == 0x42) { // Down Arrow
                cli_history_recall(-1);
                return 0;
            } else if (c == 0x43) { // Right Arrow
                if (s_cursor_pos < s_line_len) {
                    tx_raw(&s_line[s_cursor_pos], 1);
                    s_cursor_pos++;
                }
                return 0;
            } else if (c == 0x44) { // Left Arrow
                if (s_cursor_pos > 0) {
                    tx_raw("\b", 1);
                    s_cursor_pos--;
                }
                return 0;
            }
            return 0;
        }
    }

    // --- Handle Normal Keys ---

    if (c == 0x1b) { // ESC
        s_input_state = CLI_INPUT_WAIT_SPEC_KEY;
        return 0;
    }

    if (c == '\t') { // Tab
        cli_tab_complete();
        return 0;
    }

    if (c == '\b' || c == 0x7F) { // Backspace / Del
        if (s_cursor_pos > 0) {
            s_cursor_pos--;
            s_line_len--;
            // Shift characters left
            memmove(s_line + s_cursor_pos, s_line + s_cursor_pos + 1, s_line_len - s_cursor_pos + 1);

            // Echo: backspace, reprint rest of line, clear extra char, move cursor back
            echo_backspace();
            tx_raw(s_line + s_cursor_pos, s_line_len - s_cursor_pos);
            tx_raw(" ", 1);
            for (int i = 0; i <= s_line_len - s_cursor_pos; i++) echo_backspace();
        }
        return 0;
    }

    if (c == '\r' || c == '\n') {
        if (s_last_was_eol && (s_last_was_eol != (uint8_t)c)) {
            s_last_was_eol = 0;
            return 0;
        }
        s_last_was_eol = (uint8_t)c;

        tx_raw("\r\n", 2);
        if (s_ready) {
            s_line_len = 0;
            s_cursor_pos = 0;
            s_line[0]  = 0;
            tx_str("cli: input overflow, line dropped\r\n");
            return -1;
        }

        // Save to history before publishing
        cli_history_add(s_line);

        memcpy(s_ready_line, s_line, s_line_len + 1);
        s_ready     = 1;
        s_line_len  = 0;
        s_cursor_pos = 0;
        s_line[0]   = 0;
        return 0;
    } else {
        s_last_was_eol = 0;
    }

    if (c < 0x20) {
        return 0;
    }

    if (s_line_len >= CLI_LINE_MAX - 1) {
        tx_raw("\a", 1);
        return -1;
    }

    // Insert character at cursor position
    if (s_cursor_pos < s_line_len) {
        memmove(s_line + s_cursor_pos + 1, s_line + s_cursor_pos, s_line_len - s_cursor_pos + 1);
    }
    s_line[s_cursor_pos] = c;
    s_cursor_pos++;
    s_line_len++;

    // Echo the inserted character and the rest of the line
    tx_raw(&s_line[s_cursor_pos - 1], s_line_len - s_cursor_pos + 1);

    // Move cursor back to the correct position
    for (int i = 0; i < s_line_len - s_cursor_pos; i++) {
        tx_raw("\b", 1);
    }

    return 0;
}

/* --------------------------- tokenizer ----------------------------------- */

static int split_args(char *line, char *argv[], int argv_max)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < argv_max) {
        while (*p && (*p == ' ' || *p == '\t')) p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    return argc;
}

/* --------------------------- dispatcher ---------------------------------- */

static int find_and_run(char *line)
{
    static char *argv[CLI_ARGV_MAX];
    int argc = split_args(line, argv, CLI_ARGV_MAX);
    if (argc == 0) return 0;

    const char *name = argv[0];
    for (uint8_t i = 0; i < s_cmd_count; i++) {
        if (strcmp(name, s_cmds[i].name) == 0) {
            int rc = s_cmds[i].handler(argc, argv);
            if (rc != 0) {
                char tmp[32];
                int n = snprintf(tmp, sizeof(tmp), "  (rc=%d)\r\n", rc);
                if (n > 0) tx_raw(tmp, (uint16_t)n);
            }
            return 1;
        }
    }
    tx_str("  Unknown command: ");
    tx_str(name);
    tx_str(". Type 'help' to list all.\r\n");
    return 0;
}

/* --------------------------- poll task ----------------------------------- */

int cli_task(void)
{
    int executed = 0;
    if (s_ready) {
        char line[CLI_LINE_MAX];
        // __disable_irq();
        memcpy(line, s_ready_line, sizeof(line));
        s_ready = 0;
        // __enable_irq();

        if (line[0]) {
            find_and_run(line);
            executed++;
        }
        cli_print_prompt();
    }
    return executed;
}

/* --------------------------- output helpers ------------------------------ */

void cli_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_txbuf, sizeof(s_txbuf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((uint16_t)n >= sizeof(s_txbuf)) n = sizeof(s_txbuf) - 1;
    tx_raw(s_txbuf, (uint16_t)n);
}

void cli_print_prompt(void)
{
    tx_str(CLI_PROMPT);
}
