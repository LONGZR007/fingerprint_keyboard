/********************************** (C) COPYRIGHT *******************************
 * File Name          : cli_app_cmds.c
 * Description        : Example CLI commands: help / echo / info / reset /
 *                      adv / led / baud.
 *                      Accesses BLE stack state via public HAL/HIDdev APIs
 *                      (kept header-only to avoid dragging in the full
 *                      GATT/GAP type soup through hiddev.h).
 *******************************************************************************/
#include "cli.h"

#include <stdlib.h>
#include <string.h>

/* BLE & HAL headers */
#include "CONFIG.h"
#include "HAL.h"
#include "hidkbd.h"
#include "usb_composite.h"

/* ---- tiny delay / NOP helper ------------------------------------------------ */
#ifndef __NOP
#define __NOP()  __asm volatile("nop")
#endif

/* ---- parameter parser (hex/dec) ------------------------------------------- */
static long parse_num(const char *s)
{
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return strtol(s, NULL, 16);
    return strtol(s, NULL, 10);
}

/* ---- Forward declarations & BLE API wrappers using raw symbol lookup ------
 *
 * Pulling in hiddev.h transitively requires the full GAP/GATT type
 * machinery (gapRolesStateNotify_t / gattAttribute_t / bStatus_t ...) which
 * is only visible inside Profile/ compilation units.  We declare only the
 * subset we really use.  These symbols live in the BLE library/profile
 * object archives already linked by the project.
 * ------------------------------------------------------------------------- */
extern const uint8_t VER_LIB[];
extern __attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

/* GAPRole_GetParameter / _SetParameter come from CH58xBLE_LIB.h through
 * CONFIG.h above; we only need matching typedef names, not re-declaration.
 * The declared prototypes return bStatus_t (= uint8_t) and take uint16_t
 * len for SetParameter, with non-const pValue pointer. */
typedef uint8_t cli_ble_status_t;

#ifndef GAPROLE_BD_ADDR
#define GAPROLE_BD_ADDR         0x13
#endif
#ifndef GAPROLE_ADVERT_ENABLED
#define GAPROLE_ADVERT_ENABLED  0x01
#endif

/* ---- cli_uart_set_baud: implemented via HAL cli_uart.h API --------------- */
#include "cli_uart.h"
#include "CH58x_uart.h"

/* Two-level macro expansion (same pattern as in cli_uart.c) */
#define _CUC3(a,b,c)   a##b##c
#define _CUX3(a,b,c)   _CUC3(a,b,c)
#define CU_FN(suf)     _CUX3(UART, CLI_UART_PORT, suf)

void cli_uart_set_baud(uint32_t baud)
{
    CU_FN(_BaudRateCfg)(baud);
}

/* =======================================================================
 * Commands
 * ===================================================================== */

static const cli_cmd_t s_app_cmds[];   /* forward for help listing */

/* --- help --------------------------------------------------------------- */
static int cmd_help(int argc, char *argv[])
{
    (void)argc; (void)argv;
    cli_print("\r\n  Available commands:\r\n");
    for (const cli_cmd_t *c = s_app_cmds; c->name; c++) {
        cli_print("    %-12s %s\r\n", c->name, c->help ? c->help : "");
    }
    cli_print("\r\n  Usage: <command> [arg1] [arg2]...  (blank-separated, "
              "max %d args)\r\n", CLI_ARGV_MAX - 1);
    return 0;
}

/* --- echo --------------------------------------------------------------- */
static int cmd_echo(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) cli_print(" ");
        cli_print("%s", argv[i]);
    }
    cli_print("\r\n");
    return 0;
}

/* --- info --------------------------------------------------------------- */
static int cmd_info(int argc, char *argv[])
{
    (void)argc; (void)argv;
    uint8_t addr[6] = {0};
    GAPRole_GetParameter(GAPROLE_BD_ADDR, addr);
    cli_print("  LIB VER   : %s\r\n", VER_LIB);
    cli_print("  MAC       : %02X:%02X:%02X:%02X:%02X:%02X\r\n",
              addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    cli_print("  BLE heap  : %u bytes (%u KB)\r\n",
              (unsigned)BLE_MEMHEAP_SIZE,
              (unsigned)(BLE_MEMHEAP_SIZE / 1024));
    cli_print("  SYS clk   : %lu MHz\r\n",
              (unsigned long)(GetSysClock() / 1000000UL));
    cli_print("  CLI line  : %d bytes, ring  %d bytes\r\n",
              CLI_LINE_MAX, CLI_RING_SIZE);
    cli_print("  UART port : UART%d @ %u 8N1 (RX irq timeout)\r\n",
              CLI_UART_PORT, (unsigned)CLI_UART_BAUD);
    return 0;
}

/* --- reset -------------------------------------------------------------- */
static int cmd_reset(int argc, char *argv[])
{
    (void)argc; (void)argv;
    cli_print("  System reset in 200ms...\r\n");
    for (volatile uint32_t i = 0; i < 1200000UL; i++) { __NOP(); }
    SYS_ResetExecute();
    return 0;                               /* never reached */
}

/* --- adv --------------------------------------------------------------- */
static int cmd_adv(int argc, char *argv[])
{
    if (argc < 2) {
        uint8_t en = 0;
        GAPRole_GetParameter(GAPROLE_ADVERT_ENABLED, &en);
        cli_print("  advertising: %s\r\n", en ? "ON" : "OFF");
        return 0;
    }
    uint8_t en;
    if (!strcmp(argv[1], "on")  || !strcmp(argv[1], "1") || !strcmp(argv[1], "start")) en = 1;
    else if (!strcmp(argv[1], "off") || !strcmp(argv[1], "0") || !strcmp(argv[1], "stop")) en = 0;
    else {
        cli_print("  usage: adv [on|off]\r\n");
        return -1;
    }
    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(en), &en);
    cli_print("  advertising set to %s\r\n", en ? "ON" : "OFF");
    return 0;
}

/* --- led --------------------------------------------------------------- */
#define LED_PIN          GPIO_Pin_13
static uint8_t s_led_inited;

static int cmd_led(int argc, char *argv[])
{
    if (!s_led_inited) {
        GPIOA_ModeCfg(LED_PIN, GPIO_ModeOut_PP_5mA);
        GPIOA_SetBits(LED_PIN);
        s_led_inited = 1;
    }
    if (argc < 2) {
        /* toggle */
        uint32_t out = R32_PA_OUT;
        if (out & LED_PIN) {
            GPIOA_ResetBits(LED_PIN);
            cli_print("  LED ON\r\n");
        } else {
            GPIOA_SetBits(LED_PIN);
            cli_print("  LED OFF\r\n");
        }
        return 0;
    }
    if (!strcmp(argv[1], "on")  || !strcmp(argv[1], "1")) {
        GPIOA_ResetBits(LED_PIN); cli_print("  LED ON\r\n");
    } else if (!strcmp(argv[1], "off") || !strcmp(argv[1], "0")) {
        GPIOA_SetBits(LED_PIN);   cli_print("  LED OFF\r\n");
    } else {
        cli_print("  usage: led [on|off]\r\n");
        return -1;
    }
    return 0;
}

/* --- baud -------------------------------------------------------------- */
static int cmd_baud(int argc, char *argv[])
{
    if (argc < 2) {
        cli_print("  usage: baud <rate>\r\n  current: %u\r\n", (unsigned)CLI_UART_BAUD);
        return 0;
    }
    uint32_t rate = (uint32_t)parse_num(argv[1]);
    if (rate < 1200 || rate > 4000000UL) {
        cli_print("  invalid rate %lu\r\n", (unsigned long)rate);
        return -1;
    }
    cli_print("  switching baud to %lu ... (host must re-open)\r\n", (unsigned long)rate);
    /* Allow the host some time to receive this line before reconfiguring */
    for (volatile uint32_t i = 0; i < 180000UL; i++) { __NOP(); }
    cli_uart_set_baud(rate);
    return 0;
}

/* --- type -------------------------------------------------------------- */
static int cmd_type(int argc, char *argv[])
{
    if (argc < 2) {
        cli_print("  usage: type <text...>\r\n"
                  "         examples:\r\n"
                  "           type aB3\r\n"
                  "           type hello world\r\n"
                  "           type \"passw0rd!\"\r\n"
                  "         Note: sends via both BLE and USB HID keyboards.\r\n");
        return 0;
    }
    /* Rejoin argv[1..argc-1] back into one string with single spaces,
     * mirroring the behaviour the user expects when they type multiple
     * shell tokens. */
    static char s_line[CLI_LINE_MAX];
    int n = 0;
    for (int i = 1; i < argc && n < (int)(sizeof(s_line) - 1); i++) {
        if (i > 1 && n < (int)(sizeof(s_line) - 1)) {
            s_line[n++] = ' ';
        }
        for (const char *p = argv[i]; *p && n < (int)(sizeof(s_line) - 1); p++) {
            s_line[n++] = *p;
        }
    }
    s_line[n] = '\0';

    int typed = hidkbd_type_text(s_line);
    int typed_usb = usb_hid_send_text(s_line);
    cli_print("  BLE typed %d, USB typed %d of %d: \"%s\"\r\n",
              typed, typed_usb, n, s_line);
    return 0;
}

/* =======================================================================
 * Command table + registration entry
 * ===================================================================== */

static const cli_cmd_t s_app_cmds[] = {
    { "help",   cmd_help,   "list all commands" },
    { "echo",   cmd_echo,   "echo <words...>          print arguments" },
    { "type",   cmd_type,   "type <words...>          HID-type string (letters/digits/symbols)" },
    { "info",   cmd_info,   "show build / hw / BLE info" },
    { "reset",  cmd_reset,  "software reset MCU" },
    { "adv",    cmd_adv,    "adv [on|off]             control BLE advertising" },
    { "led",    cmd_led,    "led [on|off]             toggle PA13 LED" },
    { "baud",   cmd_baud,   "baud <rate>              change CLI UART baudrate" },
    { NULL,     NULL,       NULL }
};

int cli_app_cmds_register(void)
{
    return cli_register_cmds(s_app_cmds);
}
