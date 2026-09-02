/********************************** (C) COPYRIGHT *******************************
 * File Name          : prox_cli.c
 * Description        : 距离监测/自动锁屏 CLI 命令
 *                      rssi  - 读取电脑/手机链路 RSSI
 *                      prox  - 距离监测配置与状态 (on/off/show/thr/exit/
 *                              confirm/cd/lost/ch/test/pair)
 *                      修改类子命令立即生效并自动存 DataFlash。
 *********************************************************************************
 * Copyright (c) 2026
 *******************************************************************************/

/*********************************************************************
 * INCLUDES
 */
#include "CONFIG.h"
#include <string.h>
#include <stdlib.h>
#include "cli.h"
#include "hiddev.h"
#include "proximity.h"

/*********************************************************************
 * LOCALS
 */
static const char *prox_state_name(uint8_t st)
{
    switch(st)
    {
        case PROX_STATE_ARMED:     return "armed";
        case PROX_STATE_TRIGGERED: return "triggered";
        case PROX_STATE_COOLDOWN:  return "cooldown";
        case PROX_STATE_OFF:       return "off";
        default:                   return "?";
    }
}

static const char *prox_ch_name(uint8_t ch)
{
    switch(ch)
    {
        case PROX_CH_AUTO: return "auto";
        case PROX_CH_BLE:  return "ble";
        case PROX_CH_USB:  return "usb";
        case PROX_CH_BOTH: return "both";
        default:           return "?";
    }
}

static int prox_parse_ch(const char *s, uint8_t *out)
{
    if(!strcmp(s, "auto"))      *out = PROX_CH_AUTO;
    else if(!strcmp(s, "ble"))  *out = PROX_CH_BLE;
    else if(!strcmp(s, "usb"))  *out = PROX_CH_USB;
    else if(!strcmp(s, "both")) *out = PROX_CH_BOTH;
    else return -1;
    return 0;
}

/* =======================================================================
 * rssi 命令
 * ===================================================================== */
static int cmd_rssi(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* 手机(监测连接) RSSI：EMA 平滑后每 500ms 更新一次 */
    if(HidDev_MonConnActive())
    {
        cli_print("phone (ema): %d dBm   state: %s\r\n",
                  Prox_GetRssi(), prox_state_name(Prox_GetState()));
    }
    else
    {
        cli_print("phone: not connected/activated (ema n/a)   state: %s\r\n",
                  prox_state_name(Prox_GetState()));
    }

    /* 电脑(主连接) RSSI：异步读取，结果缓存供下次查看 */
    if(HidDev_MasterConnected())
    {
        HidDev_ReadMasterRssi();
        cli_print("pc (last):   %d dBm   (async read issued, run again for fresh value)\r\n",
                  Prox_GetMasterRssi());
    }
    else
    {
        cli_print("pc: not connected\r\n");
    }

    return 0;
}

/* =======================================================================
 * prox 命令
 * ===================================================================== */
static void prox_show(void)
{
    prox_cfg_t *c = Prox_GetCfg();

    cli_print("prox %s, state: %s\r\n",
              c->enabled ? "on" : "off",
              prox_state_name(Prox_GetState()));
    cli_print("  thr_enter: %d dBm   thr_exit: %d dBm\r\n",
              c->thr_enter, c->thr_exit);
    cli_print("  confirm: %us   cooldown: %us   lost_lock: %us\r\n",
              c->confirm_s, c->cooldown_s, c->lost_lock_s);
    cli_print("  channel: %s   rssi(ema): %d dBm   pc rssi: %d dBm\r\n",
              prox_ch_name(c->channel), Prox_GetRssi(), Prox_GetMasterRssi());
    cli_print("  master: %s secure=%d   monitor: %s\r\n",
              HidDev_MasterConnected() ? "connected" : "down",
              (int)HidDev_MasterSecure(),
              HidDev_MonConnActive() ? "active" : "down");
}

static int cmd_prox(int argc, char *argv[])
{
    prox_cfg_t *c = Prox_GetCfg();

    if(argc < 2)
    {
        prox_show();
        return 0;
    }

    if(!strcmp(argv[1], "show"))
    {
        prox_show();
    }
    else if(!strcmp(argv[1], "on"))
    {
        c->enabled = 1;
        cli_print("prox enabled (saved)\r\n");
        Prox_SaveCfg();
    }
    else if(!strcmp(argv[1], "off"))
    {
        c->enabled = 0;
        cli_print("prox disabled (saved)\r\n");
        Prox_SaveCfg();
    }
    else if(!strcmp(argv[1], "thr") && argc >= 3)
    {
        int8_t v = (int8_t)atoi(argv[2]);
        if(v > -30 || v < -100)
        {
            cli_print("thr_enter must be -100..-30 dBm (e.g. prox thr -70)\r\n");
            return 1;
        }
        c->thr_enter = v;
        cli_print("thr_enter = %d dBm (saved)\r\n", c->thr_enter);
        Prox_SaveCfg();
    }
    else if(!strcmp(argv[1], "exit") && argc >= 3)
    {
        int8_t v = (int8_t)atoi(argv[2]);
        if(v > -20 || v < -90)
        {
            cli_print("thr_exit must be -90..-20 dBm (e.g. prox exit -62)\r\n");
            return 1;
        }
        c->thr_exit = v;
        cli_print("thr_exit = %d dBm (saved)\r\n", c->thr_exit);
        Prox_SaveCfg();
    }
    else if(!strcmp(argv[1], "def"))
    {
        Prox_RestoreDefaults();
        cli_print("defaults restored (thr -70/-62, confirm 5s, cd 60s, ch auto) (saved)\r\n");
    }
    else if(!strcmp(argv[1], "confirm") && argc >= 3)
    {
        c->confirm_s = (uint8_t)atoi(argv[2]);
        cli_print("confirm = %us (saved)\r\n", c->confirm_s);
        Prox_SaveCfg();
    }
    else if(!strcmp(argv[1], "cd") && argc >= 3)
    {
        c->cooldown_s = (uint8_t)atoi(argv[2]);
        cli_print("cooldown = %us (saved)\r\n", c->cooldown_s);
        Prox_SaveCfg();
    }
    else if(!strcmp(argv[1], "lost") && argc >= 3)
    {
        c->lost_lock_s = (uint8_t)atoi(argv[2]);
        cli_print("lost_lock = %us%s (saved)\r\n",
                  c->lost_lock_s, c->lost_lock_s ? "" : " (disabled)");
        Prox_SaveCfg();
    }
    else if(!strcmp(argv[1], "ch") && argc >= 3)
    {
        uint8_t ch;
        if(prox_parse_ch(argv[2], &ch))
        {
            cli_print("usage: prox ch <auto|ble|usb|both>\r\n");
            return 1;
        }
        c->channel = ch;
        cli_print("channel = %s (saved)\r\n", prox_ch_name(ch));
        Prox_SaveCfg();
    }
    else if(!strcmp(argv[1], "test"))
    {
        Prox_TestTrigger();
        cli_print("Win+L sent on channel %s (state: %s)\r\n",
                  prox_ch_name(c->channel), prox_state_name(Prox_GetState()));
    }
    else if(!strcmp(argv[1], "pair"))
    {
        Prox_OpenPairing();
        cli_print("pairing window open 60s, pair the phone in system BT settings\r\n");
    }
    else
    {
        cli_print("usage: prox [show|on|off|thr <n>|exit <n>|confirm <s>|cd <s>|"
                  "lost <s>|ch <auto|ble|usb|both>|test|pair|def]\r\n");
        return 1;
    }

    return 0;
}

/* =======================================================================
 * 静态命令注册 (链接器 "cli_cmds" 段)
 * ===================================================================== */
CLI_CMD_REGISTER("rssi", cmd_rssi, "rssi                     show phone/pc link RSSI");
CLI_CMD_REGISTER("prox", cmd_prox, "prox [show|on|off|thr|exit|confirm|cd|lost|ch|test|pair] proximity lock config");
