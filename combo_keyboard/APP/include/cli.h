/********************************** (C) COPYRIGHT *******************************
 * File Name          : cli.h
 * Description        : Lightweight UART CLI core (layered, UART-agnostic)
 *******************************************************************************/
#ifndef __CLI_H__
#define __CLI_H__

#include "CH58x_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------ */
#ifndef CLI_LINE_MAX
#define CLI_LINE_MAX         128      /* max input length incl. terminator */
#endif
#ifndef CLI_ARGV_MAX
#define CLI_ARGV_MAX         10       /* max arguments per command         */
#endif
#ifndef CLI_CMD_MAX
#define CLI_CMD_MAX          16       /* max registered commands           */
#endif
#ifndef CLI_PROMPT
#define CLI_PROMPT           "> "
#endif

/* --------------------------------------------------------------------------
 * Types
 * ------------------------------------------------------------------------ */
/* Command handler signature: argc/argv, argv[0] is command name */
typedef int (*cli_cmd_fn_t)(int argc, char *argv[]);

typedef struct {
    const char     *name;      /* command name, no spaces            */
    cli_cmd_fn_t    handler;   /* handler function                   */
    const char     *help;      /* one-line usage (may be NULL)       */
} cli_cmd_t;

/* --------------------------------------------------------------------------
 * CLI core API (application/init layer calls these)
 * ------------------------------------------------------------------------ */

/* Initialize CLI core (line buffer, command table empty) */
void cli_init(void);

/* Register a command. Returns 0 on success, -1 on table full. */
int  cli_register_cmd(const char *name, cli_cmd_fn_t fn, const char *help);

/* Register an array of commands (cli_cmd_t table terminated by {NULL,NULL,NULL}) */
int  cli_register_cmds(const cli_cmd_t *table);

/* Push a byte into the CLI engine (UART HAL calls this, thread/ISR safe: simple mask).
 * Returns the byte if consumed, -1 if input buffer was full (drop). */
int  cli_rx_byte(uint8_t b);

/* Poll task: drain input, process complete lines (call from main loop / TMOS task).
 * Returns number of commands executed this tick. */
int  cli_task(void);

/* Print formatted output through UART HAL. Safe to call inside command handlers. */
void cli_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Print prompt. Called automatically after each command; explicit calls allowed. */
void cli_print_prompt(void);

/* --------------------------------------------------------------------------
 * UART HAL interface (platform provides these; CLI core consumes)
 * ------------------------------------------------------------------------ */

/* Send ONE raw byte (blocking). Provided by cli_uart.c. */
extern void cli_uart_putc(uint8_t b);

/* Send N raw bytes (blocking). Default implementation calls cli_uart_putc()
 * in a loop; HAL may override for efficiency. */
extern void cli_uart_send(const uint8_t *buf, uint16_t len);

/* Optional: HAL ready callback. CLI core will skip tx until init done. */
extern BOOL cli_uart_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* __CLI_H__ */
