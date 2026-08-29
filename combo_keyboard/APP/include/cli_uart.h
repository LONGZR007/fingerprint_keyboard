/********************************** (C) COPYRIGHT *******************************
 * File Name          : cli_uart.h
 * Description        : UART HAL for the lightweight CLI (swap port by changing
 *                      CLI_UART_PORT only).  Uses UART RX TIMEOUT interrupt
 *                      (方案 A) + a tiny ring buffer to feed cli_rx_byte().
 *******************************************************************************/
#ifndef __CLI_UART_H__
#define __CLI_UART_H__

#include "CH58x_common.h"
#include "cli.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 * PORT SELECT — change the single macro below to swap UART
 *   0 -> UART0   (TX PB7,  RX PB4)
 *   1 -> UART1   (TX PA9,  RX PA8)   <-- default, matches DEBUG=1 wiring
 *   2 -> UART2   (TX PA7,  RX PA6)
 *   3 -> UART3   (TX PA5,  RX PA4)
 * =====================================================================*/
#ifndef CLI_UART_PORT
#define CLI_UART_PORT        1
#endif

/* =====================================================================
 * CLI 串口通道选择（输入 + 输出统一由 CLI_PORT_MASK 控制）
 *   CLI_PORT_UART1 : 仅 UART1（物理串口, CLI_UART_PORT 指定）
 *   CLI_PORT_CDC   : 仅 USB CDC（ttyACM0）
 *   CLI_PORT_BOTH  : 两路同时（默认）
 * 编译时用 -DCLI_PORT_MASK=... 或直接改下面的默认值。
 * =====================================================================*/
#define CLI_PORT_UART1      0x01
#define CLI_PORT_CDC        0x02
#ifndef CLI_PORT_MASK
#define CLI_PORT_MASK       (CLI_PORT_UART1 | CLI_PORT_CDC)
#endif

/* HAL configuration */
#ifndef CLI_UART_BAUD
#define CLI_UART_BAUD        115200
#endif
#ifndef CLI_UART_TRIG
#define CLI_UART_TRIG        UART_7BYTE_TRIG
#endif
#ifndef CLI_RING_SIZE
#define CLI_RING_SIZE        256    /* MUST be power of two (mask used) */
#endif
#ifndef CLI_UART_IRQ_PRI
#define CLI_UART_IRQ_PRI     0x70   /* lower nibble is preemption; set low
                                       so BLE IRQs are not starved       */
#endif

/* --------------------------------------------------------------------------
 * HAL public API
 * ------------------------------------------------------------------------ */

/* Init pins + UART + IRQs. Safe even when DEBUG init already configured TX. */
void cli_uart_init(void);

/* Called by cli_task caller (TMOS task or main loop). Feeds ringbuf to CLI
 * engine byte-by-byte. Returns number of bytes drained this call. */
uint16_t cli_uart_drain(void);

/* How many bytes are currently buffered (informational / flow control). */
uint16_t cli_uart_available(void);

#ifdef __cplusplus
}
#endif

#endif /* __CLI_UART_H__ */
