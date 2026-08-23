/********************************** (C) COPYRIGHT *******************************
 * File Name          : cli.c
 * Description        : Lightweight CLI core:
 *                      - line buffer with backspace editing & echo
 *                      - argc/argv tokenizer (splits on spaces/tabs)
 *                      - command lookup & dispatch
 *                      - UART-agnostic: UART HAL provides cli_uart_putc /
 *                        cli_uart_send; bytes are fed in via cli_rx_byte()
 *******************************************************************************/
#include "cli.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* RISC-V PFIC interrupt control (shared across CH58x core_riscv.h) */
#ifndef __disable_irq
#define __disable_irq()  __asm volatile("csrci mstatus, 0x8")
#define __enable_irq()   __asm volatile("csrsi mstatus, 0x8")
#endif

/* --------------------------- internal state ------------------------------ */

static cli_cmd_t  s_cmds[CLI_CMD_MAX];
static uint8_t    s_cmd_count;

/* Input line being edited. Kept as NUL-terminated C string as we build it. */
static char       s_line[CLI_LINE_MAX];
static uint16_t   s_line_len;            /* current used chars excl NUL     */

/* Output scratch buffer used by cli_print (vprintf -> this -> uart send) */
static char       s_txbuf[CLI_LINE_MAX];

/* When a full line is received we copy it here so new input can start
 * immediately; cli_task() consumes s_ready_line. */
static char       s_ready_line[CLI_LINE_MAX];
static volatile uint8_t s_ready;        /* 1 when a complete line is ready */

/* For CRLF / LFCR collapsing in cli_rx_byte() (scope moved up so else
 * branch can reference it). */
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

/* Backspace cursor 1 char left using BS-SPACE-BS (works on dumb terminals). */
static void echo_backspace(void)
{
    tx_raw("\b \b", 3);
}

/* --------------------------- public API ---------------------------------- */

void cli_init(void)
{
    s_cmd_count = 0;
    s_line_len  = 0;
    s_line[0]   = 0;
    s_ready     = 0;
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
            return -1;                /* table overflow, stop early */
        }
    }
    return added;
}

/* Called from ISR or task context. Lockless single-producer/single-consumer
 * since only one ISR feeds bytes and one task drains s_ready. */
int cli_rx_byte(uint8_t b)
{
    char c = (char)b;

    /* Backspace / DEL (0x7F) */
    if (c == '\b' || c == 0x7F) {
        if (s_line_len > 0) {
            s_line_len--;
            s_line[s_line_len] = 0;
            echo_backspace();
        }
        return (int)b;
    }

    /* CR / LF -> end of line */
    if (c == '\r' || c == '\n') {
        /* Collapse CRLF / LFCR sequences -> only one line event */
        if (s_last_was_eol && (s_last_was_eol != (uint8_t)c)) {
            s_last_was_eol = 0;
            return (int)b;
        }
        s_last_was_eol = (uint8_t)c;

        tx_raw("\r\n", 2);
        if (s_ready) {
            /* Consumer hasn't picked up last line yet -> drop this one */
            s_line_len = 0;
            s_line[0]  = 0;
            tx_str("cli: input overflow, line dropped\r\n");
            return -1;
        }
        /* Publish ready line */
        memcpy(s_ready_line, s_line, s_line_len + 1);
        s_ready     = 1;
        s_line_len  = 0;
        s_line[0]   = 0;
        return (int)b;
    } else {
        s_last_was_eol = 0;
    }

    /* Ignore other non-printable (except 0x09 tab, we expand to spaces) */
    if (c == '\t') {
        /* Insert spaces for tab, stay within budget */
        static const char s_spc[] = "    ";
        uint8_t need = sizeof(s_spc) - 1;
        if (s_line_len + need >= CLI_LINE_MAX - 1) {
            tx_raw("\a", 1);                        /* BEL */
            return -1;
        }
        memcpy(&s_line[s_line_len], s_spc, need);
        s_line_len += need;
        s_line[s_line_len] = 0;
        tx_raw(s_spc, need);
        return (int)b;
    }

    if (c < 0x20) {
        return (int)b;                              /* swallow silently */
    }

    /* Ordinary printable character */
    if (s_line_len + 1 >= CLI_LINE_MAX - 1) {
        tx_raw("\a", 1);                            /* BEL: input too long */
        return -1;
    }
    s_line[s_line_len++] = c;
    s_line[s_line_len]   = 0;
    cli_uart_putc((uint8_t)c);                      /* echo */
    return (int)b;
}

/* --------------------------- tokenizer ----------------------------------- */

static int split_args(char *line, char *argv[], int argv_max)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < argv_max) {
        /* skip leading blanks */
        while (*p && (*p == ' ' || *p == '\t')) p++;
        if (!*p) break;
        argv[argc++] = p;
        /* skip until next blank */
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
    if (argc == 0) return 0;                        /* empty line, ok */

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
    /* Not found */
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
        /* Copy line out then immediately release producer slot */
        char line[CLI_LINE_MAX];
        __disable_irq();
        memcpy(line, s_ready_line, sizeof(line));
        s_ready = 0;
        __enable_irq();

        if (line[0]) {                               /* skip empty */
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
