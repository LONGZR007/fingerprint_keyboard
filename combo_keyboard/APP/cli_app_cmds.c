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
#include "keyboard_dispatch.h"

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

/* HidDev_SetParameter lives in Profile/hiddev.c; keep this file header-only
 * (same reason as above) and declare just the subset we use.
 * HIDDEV_ERASE_ALLBONDS drops the link (if connected) and erases every
 * bonded device from flash. */
extern uint8_t HidDev_SetParameter(uint8_t param, uint8_t len, void *pValue);
#ifndef HIDDEV_ERASE_ALLBONDS
#define HIDDEV_ERASE_ALLBONDS   0
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

/* --- reboot ------------------------------------------------------------- */
static int cmd_reboot(int argc, char *argv[])
{
    (void)argc; (void)argv;
    cli_print("  System reboot in 200ms...\r\n");
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
                  "         channel: %s (use 'channel' cmd to change)\r\n"
                  "         progress: %d sent, busy=%s\r\n",
                  keyboard_channel_name(keyboard_get_channel()),
                  keyboard_type_progress(),
                  keyboard_type_busy() ? "yes" : "no");
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

    if (keyboard_type_busy()) {
        cli_print("  BUSY: previous type still in flight (%d/%d done via %s)\r\n",
                  keyboard_type_progress(), n,
                  keyboard_channel_name(keyboard_get_channel()));
        return -1;
    }

    int scheduled = keyboard_type(s_line);
    if (scheduled > 0) {
        cli_print("  scheduled %d char(s) via %s: \"%s\"\r\n"
                  "  (call 'type' with no args to check progress)\r\n",
                  scheduled,
                  keyboard_channel_name(keyboard_get_channel()), s_line);
    } else {
        cli_print("  schedule FAILED (empty input or queue busy)\r\n");
        return -1;
    }
    return 0;
}

/* --- channel ----------------------------------------------------------- */
static int cmd_channel(int argc, char *argv[])
{
    if (argc < 2) {
        cli_print("  current channel: %s\r\n"
                  "  usage: channel <ble|usb|both>\r\n",
                  keyboard_channel_name(keyboard_get_channel()));
        return 0;
    }
    kbd_channel_t ch;
    if (!strcmp(argv[1], "ble") || !strcmp(argv[1], "1")) {
        ch = KBD_CH_BLE;
    } else if (!strcmp(argv[1], "usb") || !strcmp(argv[1], "2")) {
        ch = KBD_CH_USB;
    } else if (!strcmp(argv[1], "both") || !strcmp(argv[1], "3")) {
        ch = KBD_CH_BOTH;
    } else {
        cli_print("  invalid channel '%s'\r\n  usage: channel <ble|usb|both>\r\n", argv[1]);
        return -1;
    }
    keyboard_set_channel(ch);
    cli_print("  channel set to: %s\r\n", keyboard_channel_name(ch));
    return 0;
}

/* --- bond -------------------------------------------------------------- */
static const char *bond_state_name(uint8_t st)
{
    switch (st & GAPROLE_STATE_ADV_MASK)
    {
    case GAPROLE_INIT:          return "INIT";
    case GAPROLE_STARTED:       return "STARTED";
    case GAPROLE_ADVERTISING:   return "ADVERTISING";
    case GAPROLE_WAITING:       return "WAITING";
    case GAPROLE_CONNECTED:     return "CONNECTED";
    case GAPROLE_CONNECTED_ADV: return "CONNECTED_ADV";
    default:                    return "?";
    }
}

/* Parse "aa:bb:cc:dd:ee:ff" or "aabbccddeeff" into 6 bytes. */
static int bond_parse_addr(const char *s, uint8_t *out)
{
    int n = 0;
    const char *p = s;
    while (*p && n < 6) {
        unsigned v = 0;
        int k;
        for (k = 0; k < 2 && *p && *p != ':'; k++, p++) {
            char c = *p;
            if      (c >= '0' && c <= '9') v = v * 16 + (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16 + (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = v * 16 + (unsigned)(c - 'A' + 10);
            else return -1;
        }
        if (k != 2) return -1;
        out[n++] = (uint8_t)v;
        if (*p == ':') p++;
    }
    return (*p == '\0' && n == 6) ? 0 : -1;
}

static int cmd_bond(int argc, char *argv[])
{
    /* no args: report bond count + GAP state */
    if (argc < 2) {
        uint8_t cnt = 0, st = 0;
        GAPBondMgr_GetParameter(GAPBOND_BOND_COUNT, &cnt);
        GAPRole_GetParameter(GAPROLE_STATE, &st);
        cli_print("  bonded devices: %u\r\n", (unsigned)cnt);
        cli_print("  GAP state     : %s\r\n", bond_state_name(st));
        cli_print("  usage: bond <clear|count|erase [random] <addr>>\r\n");
        return 0;
    }

    if (!strcmp(argv[1], "count")) {
        uint8_t cnt = 0;
        GAPBondMgr_GetParameter(GAPBOND_BOND_COUNT, &cnt);
        cli_print("  bonded devices: %u\r\n", (unsigned)cnt);
        return 0;
    }

    if (!strcmp(argv[1], "clear")) {
        uint8_t st = 0;
        GAPRole_GetParameter(GAPROLE_STATE, &st);
        cli_print("  state: %s -> erasing all bonds", bond_state_name(st));
        if ((st & GAPROLE_STATE_ADV_MASK) == GAPROLE_CONNECTED ||
            (st & GAPROLE_STATE_ADV_MASK) == GAPROLE_CONNECTED_ADV) {
            cli_print(" (link will be dropped)");
        }
        cli_print("\r\n");

        /* Drop the link if connected and erase all bonded devices from
         * flash.  On disconnect the HID app state callback restarts
         * advertising, so the device re-enters "waiting for pairing". */
        HidDev_SetParameter(HIDDEV_ERASE_ALLBONDS, 0, NULL);

        /* Belt & braces: make sure advertising comes back up so the
         * device is immediately discoverable / pairable again. */
        {
            uint8_t adv = 1;
            GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &adv);
        }
        return 0;
    }

    if (!strcmp(argv[1], "erase")) {
        if (argc < 3) {
            cli_print("  usage: bond erase [random] <aa:bb:cc:dd:ee:ff>\r\n");
            return -1;
        }
        int type = ADDRTYPE_PUBLIC;
        const char *mac = argv[2];
        if (!strcmp(argv[2], "random") || !strcmp(argv[2], "static")) {
            type = ADDRTYPE_STATIC;
            if (argc < 4) {
                cli_print("  usage: bond erase [random] <aa:bb:cc:dd:ee:ff>\r\n");
                return -1;
            }
            mac = argv[3];
        }
        uint8_t entry[7];
        entry[0] = (uint8_t)type;
        if (bond_parse_addr(mac, &entry[1]) != 0) {
            cli_print("  bad address '%s' (use aa:bb:cc:dd:ee:ff)\r\n", mac);
            return -1;
        }
        GAPBondMgr_SetParameter(GAPBOND_ERASE_SINGLEBOND, sizeof(entry), entry);
        cli_print("  erased bond for %s%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                  type == ADDRTYPE_STATIC ? "random " : "",
                  entry[1], entry[2], entry[3], entry[4], entry[5], entry[6]);
        return 0;
    }

    cli_print("  usage: bond <clear|count|erase [random] <addr>>\r\n");
    return -1;
}

/* =======================================================================
 * Static command registration via linker section ("cli_cmds").
 * No runtime registration call needed: cli core discovers these through
 * __start_cli_cmds / __stop_cli_cmds at lookup time. The built-in 'help'
 * (registered dynamically in cli_init) lists these too.
 * ===================================================================== */

CLI_CMD_REGISTER("echo",    cmd_echo,    "echo <words...>          print arguments");
CLI_CMD_REGISTER("type",    cmd_type,    "type <words...>          HID-type string (letters/digits/symbols)");
CLI_CMD_REGISTER("channel", cmd_channel, "channel [ble|usb|both]   set HID output channel");
CLI_CMD_REGISTER("bond",    cmd_bond,    "bond <clear|count|erase> manage BLE bonding / re-enter pairing");
CLI_CMD_REGISTER("info",    cmd_info,    "show build / hw / BLE info");
CLI_CMD_REGISTER("reboot",  cmd_reboot,  "software reboot MCU");
CLI_CMD_REGISTER("adv",     cmd_adv,     "adv [on|off]             control BLE advertising");
CLI_CMD_REGISTER("led",     cmd_led,     "led [on|off]             toggle PA13 LED");
CLI_CMD_REGISTER("baud",    cmd_baud,    "baud <rate>              change CLI UART baudrate");

