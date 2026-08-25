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

/*********************************************************************
 * GLOBAL TYPEDEFS
 */
__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

#if(defined(BLE_MAC)) && (BLE_MAC == TRUE)
const uint8_t MacAddr[6] = {0x84, 0xC2, 0xE4, 0x03, 0x02, 0x02};
#endif

/* --------------------------------------------------------------------------
 * CLI integration (TMOS task, 5 ms poll interval)
 * ------------------------------------------------------------------------ */
#define CLI_TASK_EVT_POLL     0x0001
#define CLI_POLL_MS           5
static uint8_t cli_task_id = INVALID_TASK_ID;
extern int   cli_app_cmds_register(void);

static uint16_t CliTask_ProcessEvent(uint8_t task_id, uint16_t events)
{
    (void)task_id;
    if (events & CLI_TASK_EVT_POLL) {
        /* Drain UART ring -> line edit -> run command if line ready */
        cli_uart_drain();
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
    cli_app_cmds_register();
    cli_task_id = TMOS_ProcessEventRegister(CliTask_ProcessEvent);
    cli_print("\r\n=== CLI ready on UART%d %u 8N1 ===\r\n",
              (int)1 /* CLI_UART_PORT */, (unsigned)CLI_UART_BAUD);
    cli_print("Type 'help' for a list of commands.\r\n");
    cli_print_prompt();
    /* kick off periodic drain */
    tmos_start_task(cli_task_id, CLI_TASK_EVT_POLL, CLI_POLL_MS);
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
    /* USB composite device (HID keyboard + CDC serial) */
    usb_composite_init();
    cli_print("USB composite device initialized.\r\n");
    Main_Circulation();
}

/******************************** endfile @ main ******************************/
