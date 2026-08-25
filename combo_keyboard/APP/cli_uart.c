/********************************** (C) COPYRIGHT *******************************
 * File Name          : cli_uart.c
 * Description        : UART HAL for CLI
 *                      - Switch UART via CLI_UART_PORT in cli_uart.h
 *                      - RX uses TIMEOUT interrupt (方案 A) to frame bytes
 *                      - Interrupt pushes a small RING; cli_uart_drain()
 *                        (called from task context) pops bytes & feeds
 *                        cli_rx_byte() which edits the line buffer
 *                      - TX is blocking, single-byte (printf usage, not
 *                        high-throughput)
 *******************************************************************************/
#include "cli_uart.h"

#include <string.h>

/* RISC-V PFIC interrupt enable/disable (mirrors cli.c definition, local) */
#ifndef __disable_irq
#define __disable_irq()  __asm volatile("csrci mstatus, 0x8")
#define __enable_irq()   __asm volatile("csrsi mstatus, 0x8")
#endif

/* =======================================================================
 * Macro helpers: expand UARTn_* to the right API based on CLI_UART_PORT
 * ===================================================================== */
#define _U_CAT2(a, b)      a##b
#define _U_CAT3(a, b, c)   a##b##c
#define _U_CAT4(a, b, c, d) a##b##c##d
/* Two levels of indirection so that CLI_UART_PORT (a macro integer) is
 * expanded before being concatenated into the identifier. */
#define _UX_CAT3(a, b, c)   _U_CAT3(a, b, c)
#define _UX_CAT4(a, b, c, d) _U_CAT4(a, b, c, d)

#define U(n, suf)          _UX_CAT3(UART, n, suf)
#define U_R8(suf)          _UX_CAT3(R8_UART, CLI_UART_PORT, suf)
#define U_R16(suf)         _UX_CAT4(R16_UART, CLI_UART_PORT, _, suf)
#define U_R32(suf)         _UX_CAT4(R32_UART, CLI_UART_PORT, _, suf)
#define U_FN(suf)          U(CLI_UART_PORT, suf)

/* Direct register access for RFC/TFC/THR/RBR (suf is "_RFC", "_TFC",
 * "_THR", "_RBR") — the R8_UART1_THR macro uses full name so we need the
 * second level macro expansion. U_R8 above already does that. The names
 * "_RFC" etc. map to the register suffix in SFR header. */

/* SendByte / RecvByte function-style macros exist per port too. Use: */
#define U_SendByte(b)     U_FN(_SendByte(b))
#define U_RecvByte()      U_FN(_RecvByte())

/* =======================================================================
 * Port → pin mapping (primary pins; remaps not used here)
 * ===================================================================== */
#if   CLI_UART_PORT == 0
    #define CLI_TX_PIN       bTXD0
    #define CLI_RX_PIN       bRXD0
    #define CLI_TX_PORT      GPIOB             /* TX PB7 */
    #define CLI_RX_PORT      GPIOB             /* RX PB4 */
    #define CLI_TX_SETHIGH() GPIOB_SetBits(CLI_TX_PIN)
    #define CLI_TX_MODECFG() GPIOB_ModeCfg(CLI_TX_PIN, GPIO_ModeOut_PP_5mA)
    #define CLI_RX_MODECFG() GPIOB_ModeCfg(CLI_RX_PIN, GPIO_ModeIN_PU)
    #define CLI_UART_IRQn    UART0_IRQn
    #define CLI_UART_IRQHandler UART0_IRQHandler
#elif CLI_UART_PORT == 1
    #define CLI_TX_PIN       bTXD1
    #define CLI_RX_PIN       bRXD1
    #define CLI_TX_PORT      GPIOA             /* TX PA9 */
    #define CLI_RX_PORT      GPIOA             /* RX PA8 */
    #define CLI_TX_SETHIGH() GPIOA_SetBits(CLI_TX_PIN)
    #define CLI_TX_MODECFG() GPIOA_ModeCfg(CLI_TX_PIN, GPIO_ModeOut_PP_5mA)
    #define CLI_RX_MODECFG() GPIOA_ModeCfg(CLI_RX_PIN, GPIO_ModeIN_PU)
    #define CLI_UART_IRQn    UART1_IRQn
    #define CLI_UART_IRQHandler UART1_IRQHandler
#elif CLI_UART_PORT == 2
    #define CLI_TX_PIN       bTXD2
    #define CLI_RX_PIN       bRXD2
    #define CLI_TX_PORT      GPIOA             /* TX PA7 */
    #define CLI_RX_PORT      GPIOA             /* RX PA6 */
    #define CLI_TX_SETHIGH() GPIOA_SetBits(CLI_TX_PIN)
    #define CLI_TX_MODECFG() GPIOA_ModeCfg(CLI_TX_PIN, GPIO_ModeOut_PP_5mA)
    #define CLI_RX_MODECFG() GPIOA_ModeCfg(CLI_RX_PIN, GPIO_ModeIN_PU)
    #define CLI_UART_IRQn    UART2_IRQn
    #define CLI_UART_IRQHandler UART2_IRQHandler
#elif CLI_UART_PORT == 3
    #define CLI_TX_PIN       bTXD3
    #define CLI_RX_PIN       bRXD3
    #define CLI_TX_PORT      GPIOA             /* TX PA5 */
    #define CLI_RX_PORT      GPIOA             /* RX PA4 */
    #define CLI_TX_SETHIGH() GPIOA_SetBits(CLI_TX_PIN)
    #define CLI_TX_MODECFG() GPIOA_ModeCfg(CLI_TX_PIN, GPIO_ModeOut_PP_5mA)
    #define CLI_RX_MODECFG() GPIOA_ModeCfg(CLI_RX_PIN, GPIO_ModeIN_PU)
    #define CLI_UART_IRQn    UART3_IRQn
    #define CLI_UART_IRQHandler UART3_IRQHandler
#else
    #error "CLI_UART_PORT must be 0..3"
#endif

/* =======================================================================
 * Ring buffer
 * ===================================================================== */

#if (CLI_RING_SIZE & (CLI_RING_SIZE - 1))
#error "CLI_RING_SIZE must be a power of two"
#endif

static uint8_t  s_ring[CLI_RING_SIZE];
static volatile uint16_t s_r_head;   /* ISR writes at head */
static volatile uint16_t s_r_tail;   /* Task  reads at tail */

static BOOL     s_ready;

#define RING_MASK      (CLI_RING_SIZE - 1)

static uint16_t ring_avail(void)
{
    return (uint16_t)(s_r_head - s_r_tail) & RING_MASK;
}

/* Returns number of bytes pushed (0 if full). Called from ISR. */
static uint16_t ring_push(const uint8_t *data, uint16_t len)
{
    uint16_t count = 0;
    while (len--) {
        uint16_t next = (uint16_t)(s_r_head + 1) & RING_MASK;
        if (next == s_r_tail) break;           /* full */
        s_ring[s_r_head] = *data++;
        s_r_head = next;
        count++;
    }
    return count;
}

/* Called from task context, IRQs may preempt during execution (head mutates).
 * Single-byte pop is fine; use critical section for one read to be safe. */
static int ring_pop(void)
{
    if (s_r_head == s_r_tail) return -1;
    __disable_irq();
    uint8_t b = s_ring[s_r_tail];
    s_r_tail = (uint16_t)(s_r_tail + 1) & RING_MASK;
    __enable_irq();
    return (int)b;
}

/* =======================================================================
 * ISR — UART TIMEOUT + RECV_RDY → ring_push
 * ===================================================================== */

__INTERRUPT
__HIGH_CODE
void CLI_UART_IRQHandler(void)
{
    uint8_t iir = U_FN(_GetITFlag)();

    switch (iir) {
    case UART_II_RECV_TOUT:                        /* --- FIFO timeout (end of frame) --- */
        /* fallthrough: both TOUT and RECV_RDY drain the FIFO the same way */
    case UART_II_RECV_RDY:                         /* --- FIFO threshold reached --- */
    {
        uint16_t n = 0;
        uint8_t  tmp[8];
        /* RFC is FIFO count (0..UART_FIFO_SIZE); read in chunks */
        while (U_R8(_RFC)) {
            uint8_t c = U_RecvByte();
            tmp[n++] = c;
            if (n == sizeof(tmp)) {
                ring_push(tmp, n);
                n = 0;
            }
        }
        if (n) ring_push(tmp, n);
        break;
    }
    case UART_II_LINE_STAT:                        /* --- line status error --- */
    {
        volatile uint8_t stat = U_FN(_GetLinSTA)();
        (void)stat;                                 /* clear flag */
        break;
    }
    case UART_II_THR_EMPTY:
    default:
        /* THR empty / modem change (UART0 only) — not used for RX path */
        break;
    }
}

/* =======================================================================
 * Init
 * ===================================================================== */

void cli_uart_init(void)
{
    /* 1. TX pin (idle HIGH before enable to avoid framing glitch on host) */
    CLI_TX_SETHIGH();
    CLI_TX_MODECFG();

    /* 2. RX pin — pull-up */
    CLI_RX_MODECFG();

    /* 3. UART default init (baud, FIFO on, 8N1) + override baud */
    U_FN(_DefInit)();
    U_FN(_BaudRateCfg)(CLI_UART_BAUD);

    /* 4. FIFO trigger + enable interrupts: RECV_RDY + RX line status.
     *    TIMEOUT needs FIFO + at least 1 byte which triggers it implicitly. */
    U_FN(_ByteTrigCfg)(CLI_UART_TRIG);

    /* Set priority below BLE interrupts, then enable the IRQ vector. */
    PFIC_SetPriority(CLI_UART_IRQn, CLI_UART_IRQ_PRI);
    U_FN(_INTCfg)(ENABLE, RB_IER_RECV_RDY | RB_IER_LINE_STAT);
    PFIC_EnableIRQ(CLI_UART_IRQn);

    s_ready = TRUE;
}

/* =======================================================================
 * Drain — consume ring buffer into cli_rx_byte().
 * Called from the task context.
 * ===================================================================== */

uint16_t cli_uart_drain(void)
{
    uint16_t n = 0;
    int c;
    while ((c = ring_pop()) >= 0) {
        cli_rx_byte((uint8_t)c);
        n++;
    }
    return n;
}

uint16_t cli_uart_available(void)
{
    return ring_avail();
}

/* =======================================================================
 * TX side — blocking single byte out the same UART that CLI_RX uses.
 * Implements cli_uart_putc() which is called by CLI core for echo + prints.
 * ===================================================================== */

BOOL cli_uart_ready(void) { return s_ready; }

void cli_uart_putc(uint8_t b)
{
    if (!s_ready) return;
    /* Wait until TX FIFO is not full. */
    while (U_R8(_TFC) == UART_FIFO_SIZE) { /* spin */ }
    U_SendByte(b);
    if (b == '\n') {                       /* implicit CR for \n */
        while (U_R8(_TFC) == UART_FIFO_SIZE) { /* spin */ }
        U_SendByte('\r');
    }
}
