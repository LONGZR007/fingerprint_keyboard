#ifndef __CLI_H__
#define __CLI_H__

#include <stdint.h>

/* ======================== 配置区 ======================== */
#ifndef CLI_LINE_MAX
#define CLI_LINE_MAX    128
#endif

#ifndef CLI_ARGV_MAX
#define CLI_ARGV_MAX    8
#endif

#ifndef CLI_CMD_MAX
#define CLI_CMD_MAX     10      /* 动态注册的最大命令数 */
#endif

#ifndef CLI_PROMPT
#define CLI_PROMPT      "# "
#endif

#define CLI_ENABLE_LINKER_CMD_REGISTRATION 1     /* 是否启用静态命令注册 1:启用 0:禁用 */

/* ======================== 类型定义 ======================== */
typedef int (*cli_cmd_fn_t)(int argc, char *argv[]);

/* 动态注册使用的结构体（存在 RAM 数组中） */
typedef struct {
    const char *name;
    cli_cmd_fn_t handler;
    const char *help;
} cli_cmd_t;

/* 静态注册使用的结构体（存在 Flash/ROM 的特定 Section 中） */
typedef struct {
    const char *name;
    cli_cmd_fn_t handler;
    const char *help;
} cli_cmd_entry_t;

/* ======================== 静态注册宏 ======================== */
/**
 * @brief  静态注册命令宏
 * @note   利用 GCC __attribute__((section)) 将命令结构体放入 "cli_cmds" 段
 *         used 属性防止链接器优化掉未直接引用的变量
 */
#if CLI_ENABLE_LINKER_CMD_REGISTRATION
#if defined(__GNUC__) && !defined(__llvm__)
    #define CLI_CMD_REGISTER(cmd_name, cmd_handler, cmd_help) \
        static const cli_cmd_entry_t _cli_cmd_##cmd_handler \
        __attribute__((used, section("cli_cmds"))) = { \
            .name = cmd_name, \
            .handler = cmd_handler, \
            .help = cmd_help \
        }
#else
    /* 非 GCC 编译器回退到动态注册 */
    #define CLI_CMD_REGISTER(cmd_name, cmd_handler, cmd_help) \
        /* 需要用户在初始化时手动调用 cli_register_cmd */
#endif
#endif

/* ======================== API 声明 ======================== */
void cli_init(void);
int  cli_register_cmd(const char *name, cli_cmd_fn_t fn, const char *help);
int  cli_register_cmds(const cli_cmd_t *table);
void cli_print_prompt(void);
void cli_print(const char *fmt, ...);
void cli_print_help(void);
int  cli_rx_byte(uint8_t b);
int  cli_task(void);

/* --------------------------------------------------------------------------
 * UART HAL interface (platform provides; CLI core consumes via cli_tx_raw)
 * ------------------------------------------------------------------------ */
/* Send N raw bytes (blocking). Strong impl in cli_uart.c overrides weak default. */
extern void cli_tx_raw(const char *data, uint16_t len);

#endif /* __CLI_H__ */
