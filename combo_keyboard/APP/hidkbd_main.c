/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2020/08/06
 * Description        : 蓝牙键盘应用主函数及任务系统初始化
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/******************************************************************************/
/* 头文件包含 */
#include "CONFIG.h"
#include "HAL.h"
#include "hiddev.h"
#include "hidkbd.h"
#include "cli.h"
#include "cli_uart.h"
#include "usb_composite.h"
#include "fp_uart.h"
#include "fp_proto.h"
#include "fp_sm.h"
#include "user_flash.h"
#include "proximity.h"

/*********************************************************************
 * GLOBAL TYPEDEFS
 */
__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

/* 双连接(电脑HID + 手机监测)要求从机最大连接数 >= 2。
 * 若编译期在这里报错，说明 PERIPHERAL_MAX_CONNECTION 未按 CONFIG.h 生效。 */
#if PERIPHERAL_MAX_CONNECTION < 2
#error "PERIPHERAL_MAX_CONNECTION must be >= 2 (see HAL/include/CONFIG.h)"
#endif

#if(defined(BLE_MAC)) && (BLE_MAC == TRUE)
const uint8_t MacAddr[6] = {0x84, 0xC2, 0xE4, 0x03, 0x02, 0x02};
#endif

/* --------------------------------------------------------------------------
 * CLI integration (TMOS task, 5 ms poll interval)
 * ------------------------------------------------------------------------ */
#define CLI_TASK_EVT_POLL     0x0001
#define CLI_POLL_MS           5
static uint8_t cli_task_id = INVALID_TASK_ID;

static uint16_t CliTask_ProcessEvent(uint8_t task_id, uint16_t events)
{
    (void)task_id;
    if (events & CLI_TASK_EVT_POLL) {
#if (CLI_PORT_MASK & CLI_PORT_UART1)
        /* Drain UART ring -> line edit -> run command if line ready */
        cli_uart_drain();
#endif
#if (CLI_PORT_MASK & CLI_PORT_CDC)
        /* Drain USB CDC serial (ttyACM0) into the same CLI line editor */
        {
            uint8_t cdc_buf[64];
            int n;
            while ((n = usb_cdc_recv(cdc_buf, sizeof(cdc_buf))) > 0) {
                int i;
                for (i = 0; i < n; i++) cli_rx_byte(cdc_buf[i]);
            }
        }
#endif
        cli_task();
        /* re-arm the poll tick */
        tmos_start_task(cli_task_id, CLI_TASK_EVT_POLL, CLI_POLL_MS);
        return (uint16_t)(events ^ CLI_TASK_EVT_POLL);
    }
    return 0;
}

static void Cli_Init(void)
{
    cli_uart_init();
    cli_init();
    cli_task_id = TMOS_ProcessEventRegister(CliTask_ProcessEvent);
#if (CLI_PORT_MASK == CLI_PORT_UART1)
    cli_print("\r\n=== CLI ready on UART%d %u 8N1 ===\r\n",
              (int)CLI_UART_PORT, (unsigned)CLI_UART_BAUD);
#elif (CLI_PORT_MASK == CLI_PORT_CDC)
    cli_print("\r\n=== CLI ready on USB CDC (ttyACM0) ===\r\n");
#else
    cli_print("\r\n=== CLI ready on UART%d + USB CDC ===\r\n",
              (int)CLI_UART_PORT);
#endif
    cli_print("Type 'help' for a list of commands.\r\n");
    cli_print_prompt();
    /* kick off periodic drain */
    tmos_start_task(cli_task_id, CLI_TASK_EVT_POLL, CLI_POLL_MS);
}

/* --------------------------------------------------------------------------
 * Fingerprint module integration (TMOS task, 5 ms poll interval)
 * ------------------------------------------------------------------------ */
#define FP_TASK_EVT_POLL     0x0001
#define FP_POLL_MS           5
static uint8_t fp_task_id = INVALID_TASK_ID;
extern void fp_app_cmds_init(void);  /* 定义在 fp_app_cmds.c */

static uint16_t FpTask_ProcessEvent(uint8_t task_id, uint16_t events)
{
    (void)task_id;
    if (events & FP_TASK_EVT_POLL) {
        /* 串口缓冲出队 -> 协议解析 -> 状态机推进 */
        fp_uart_drain();
        fp_proto_task();
        fp_sm_task();
        /* 重新启动周期任务, 实现 5ms 周期调用 */
        tmos_start_task(fp_task_id, FP_TASK_EVT_POLL, FP_POLL_MS);
        return (uint16_t)(events ^ FP_TASK_EVT_POLL);
    }
    return 0;
}

void Fp_Init(void)
{
    fp_uart_init();        /* 初始化 UART3 HAL */
    fp_proto_init();       /* 初始化协议解析层 */
    fp_sm_init();          /* 初始化状态机 */
    fp_app_cmds_init();    /* 注册 CLI 指纹命令和通知回调 */
    /* 注册 TMOS 周期任务处理函数 */
    fp_task_id = TMOS_ProcessEventRegister(FpTask_ProcessEvent);
    /* 启动周期任务, 5ms 间隔 */
    tmos_start_task(fp_task_id, FP_TASK_EVT_POLL, FP_POLL_MS);
}

/*********************************************************************
 * @fn      Main_Circulation
 *
 * @brief   主循环
 *
 * @return  none
 */
__HIGH_CODE
__attribute__((noinline))
void Main_Circulation()
{
    while(1)
    {
        TMOS_SystemProcess();
        usb_composite_task();
    }
}

/*********************************************************************
 * @fn      main
 *
 * @brief   主函数
 *
 * @return  none
 */
int main(void)
{
#if(defined(DCDC_ENABLE)) && (DCDC_ENABLE == TRUE)
    PWR_DCDCCfg(ENABLE);
#endif
    SetSysClock(CLK_SOURCE_PLL_60MHz);
#if(defined(HAL_SLEEP)) && (HAL_SLEEP == TRUE)
    GPIOA_ModeCfg(GPIO_Pin_All, GPIO_ModeIN_PU);
    GPIOB_ModeCfg(GPIO_Pin_All, GPIO_ModeIN_PU);
#endif
#ifdef DEBUG
    GPIOA_SetBits(bTXD1);
    GPIOA_ModeCfg(bTXD1, GPIO_ModeOut_PP_5mA);
    UART1_DefInit();
#endif
    PRINT("%s\n", VER_LIB);
    CH58X_BLEInit();
    HAL_Init();
    GAPRole_PeripheralInit();
    HidDev_Init();
    HidEmu_Init();
    Cli_Init();
    Fp_Init();               /* 指纹模组驱动初始化 */
    user_flash_init();       /* 用户数据存储模块初始化 (user 命令已由链接器段静态注册) */
    Prox_Init();             /* 手机距离监测 + 离开自动锁屏 (Win+L) */
    /* USB composite device (HID keyboard + CDC serial) */
    usb_composite_init();
    cli_print("USB composite device initialized.\r\n");
    Main_Circulation();
}

/******************************** endfile @ main ******************************/
