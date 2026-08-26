/********************************** (C) COPYRIGHT *******************************
 * File Name          : fp_uart.h
 * Description        : UART HAL for the fingerprint module (swap port by
 *                      changing FP_UART_PORT only).  Mirrors cli_uart.h
 *                      layering: RX TIMEOUT interrupt (方案 A) + tiny ring
 *                      buffer feeding fp_proto_rx_byte() byte-by-byte.
 *
 *                      Default wiring:
 *                        UART3 (TX PA5 / RX PA4) @ 57600 bps
 *                      which matches the fingerprint module factory default.
 *******************************************************************************/
#ifndef __FP_UART_H__
#define __FP_UART_H__

#include "CH58x_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 * PORT SELECT — change the single macro below to swap UART
 *   0 -> UART0   (TX PB7,  RX PB4)
 *   1 -> UART1   (TX PA9,  RX PA8)
 *   2 -> UART2   (TX PA7,  RX PA6)
 *   3 -> UART3   (TX PA5,  RX PA4)   <-- default, matches FP module wiring
 * =====================================================================*/
#ifndef FP_UART_PORT
#define FP_UART_PORT         3
#endif

/* HAL configuration */
#ifndef FP_UART_BAUD
#define FP_UART_BAUD         57600   /* fingerprint module factory default */
#endif
#ifndef FP_UART_TRIG
#define FP_UART_TRIG         UART_7BYTE_TRIG
#endif
#ifndef FP_RING_SIZE
#define FP_RING_SIZE         256     /* MUST be power of two (mask used) */
#endif
#ifndef FP_UART_IRQ_PRI
#define FP_UART_IRQ_PRI      0x70    /* lower nibble is preemption; set low
                                        so BLE IRQs are not starved        */
#endif

/* --------------------------------------------------------------------------
 * Port → pin / IRQ mapping (primary pins; remaps not used here).
 * Kept in the header so user code can refer to FP_UART_IRQHandler etc.
 * ------------------------------------------------------------------------ */
#if   FP_UART_PORT == 0
    #define FP_TX_PIN           bTXD0
    #define FP_RX_PIN           bRXD0
    #define FP_TX_PORT          GPIOB             /* TX PB7 */
    #define FP_RX_PORT          GPIOB             /* RX PB4 */
    #define FP_TX_SETHIGH()     GPIOB_SetBits(FP_TX_PIN)
    #define FP_TX_MODECFG()     GPIOB_ModeCfg(FP_TX_PIN, GPIO_ModeOut_PP_5mA)
    #define FP_RX_MODECFG()     GPIOB_ModeCfg(FP_RX_PIN, GPIO_ModeIN_PU)
    #define FP_UART_IRQn        UART0_IRQn
    #define FP_UART_IRQHandler  UART0_IRQHandler
#elif FP_UART_PORT == 1
    #define FP_TX_PIN           bTXD1
    #define FP_RX_PIN           bRXD1
    #define FP_TX_PORT          GPIOA             /* TX PA9 */
    #define FP_RX_PORT          GPIOA             /* RX PA8 */
    #define FP_TX_SETHIGH()     GPIOA_SetBits(FP_TX_PIN)
    #define FP_TX_MODECFG()     GPIOA_ModeCfg(FP_TX_PIN, GPIO_ModeOut_PP_5mA)
    #define FP_RX_MODECFG()     GPIOA_ModeCfg(FP_RX_PIN, GPIO_ModeIN_PU)
    #define FP_UART_IRQn        UART1_IRQn
    #define FP_UART_IRQHandler  UART1_IRQHandler
#elif FP_UART_PORT == 2
    #define FP_TX_PIN           bTXD2
    #define FP_RX_PIN           bRXD2
    #define FP_TX_PORT          GPIOA             /* TX PA7 */
    #define FP_RX_PORT          GPIOA             /* RX PA6 */
    #define FP_TX_SETHIGH()     GPIOA_SetBits(FP_TX_PIN)
    #define FP_TX_MODECFG()     GPIOA_ModeCfg(FP_TX_PIN, GPIO_ModeOut_PP_5mA)
    #define FP_RX_MODECFG()     GPIOA_ModeCfg(FP_RX_PIN, GPIO_ModeIN_PU)
    #define FP_UART_IRQn        UART2_IRQn
    #define FP_UART_IRQHandler  UART2_IRQHandler
#elif FP_UART_PORT == 3
    #define FP_TX_PIN           bTXD3
    #define FP_RX_PIN           bRXD3
    #define FP_TX_PORT          GPIOA             /* TX PA5 */
    #define FP_RX_PORT          GPIOA             /* RX PA4 */
    #define FP_TX_SETHIGH()     GPIOA_SetBits(FP_TX_PIN)
    #define FP_TX_MODECFG()     GPIOA_ModeCfg(FP_TX_PIN, GPIO_ModeOut_PP_5mA)
    #define FP_RX_MODECFG()     GPIOA_ModeCfg(FP_RX_PIN, GPIO_ModeIN_PU)
    #define FP_UART_IRQn        UART3_IRQn
    #define FP_UART_IRQHandler  UART3_IRQHandler
#else
    #error "FP_UART_PORT must be 0..3"
#endif

/* --------------------------------------------------------------------------
 * HAL public API
 * ------------------------------------------------------------------------ */

/* Init pins + UART + IRQs. Idempotent w.r.t. pin/GPIO state. */
void fp_uart_init(void);

/* Called by the FP task (TMOS task or main loop). Pops the ring buffer
 * byte-by-byte and feeds each byte to fp_proto_rx_byte() (declared extern
 * by fp_proto.h / fp_proto.c — not #included here to avoid circular deps).
 * Returns the number of bytes drained this call. */
uint16_t fp_uart_drain(void);

/* Blocking TX of an arbitrary buffer out the FP UART. Used to push full
 * protocol frames (header + payload + checksum) atomically. */
void fp_uart_send(const uint8_t *buf, uint16_t len);

/* How many bytes are currently buffered (informational / flow control). */
uint16_t fp_uart_available(void);

/* ISR prototype — definition lives in fp_uart.c; named FP_UART_IRQHandler
 * (e.g. UART3_IRQHandler) so the startup vector picks it up automatically. */
void FP_UART_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* __FP_UART_H__ */
