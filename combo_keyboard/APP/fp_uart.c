/********************************** (C) COPYRIGHT *******************************
 * File Name          : fp_uart.c
 * Description        : UART HAL for the fingerprint module
 *                      - Switch UART via FP_UART_PORT in fp_uart.h
 *                      - RX uses TIMEOUT interrupt (方案 A) to frame bytes
 *                      - Interrupt pushes a small RING; fp_uart_drain()
 *                        (called from task context) pops bytes & feeds
 *                        fp_proto_rx_byte() (extern, see fp_proto.h)
 *                      - TX is blocking, multi-byte (fp_uart_send) for
 *                        full protocol frames
 *******************************************************************************/
#include "fp_uart.h"

#include <string.h>

/* fp_proto_rx_byte() is implemented in fp_proto.c. Declared extern here
 * (not #include "fp_proto.h") to avoid a circular header dependency. */
extern void fp_proto_rx_byte(uint8_t b);

/* RISC-V PFIC interrupt enable/disable (mirrors cli_uart.c definition) */
#ifndef __disable_irq
#define __disable_irq()  __asm volatile("csrci mstatus, 0x8")
#define __enable_irq()   __asm volatile("csrsi mstatus, 0x8")
#endif

/* =======================================================================
 * Macro helpers: expand UARTn_* to the right API based on FP_UART_PORT
 * ===================================================================== */
#define _U_CAT2(a, b)      a##b
#define _U_CAT3(a, b, c)   a##b##c
#define _U_CAT4(a, b, c, d) a##b##c##d
/* Two levels of indirection so that FP_UART_PORT (a macro integer) is
 * expanded before being concatenated into the identifier. */
#define _UX_CAT3(a, b, c)   _U_CAT3(a, b, c)
#define _UX_CAT4(a, b, c, d) _U_CAT4(a, b, c, d)

#define U(n, suf)          _UX_CAT3(UART, n, suf)
#define U_R8(suf)          _UX_CAT3(R8_UART, FP_UART_PORT, suf)
#define U_R16(suf)         _UX_CAT4(R16_UART, FP_UART_PORT, _, suf)
#define U_R32(suf)         _UX_CAT4(R32_UART, FP_UART_PORT, _, suf)
#define U_FN(suf)          U(FP_UART_PORT, suf)

/* SendByte / RecvByte function-style macros exist per port too. Use: */
#define U_SendByte(b)     U_FN(_SendByte(b))
#define U_RecvByte()      U_FN(_RecvByte())

/* =======================================================================
 * Ring buffer
 * ===================================================================== */

#if (FP_RING_SIZE & (FP_RING_SIZE - 1))
#error "FP_RING_SIZE must be a power of two"
#endif

static uint8_t  s_ring[FP_RING_SIZE];
static volatile uint16_t s_r_head;   /* ISR writes at head */
static volatile uint16_t s_r_tail;   /* Task  reads at tail */

static BOOL     s_ready;

#define RING_MASK      (FP_RING_SIZE - 1)

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
    // __disable_irq();
    uint8_t b = s_ring[s_r_tail];
    s_r_tail = (uint16_t)(s_r_tail + 1) & RING_MASK;
    // __enable_irq();
    return (int)b;
}

/* =======================================================================
 * ISR — UART TIMEOUT + RECV_RDY → ring_push
 * ===================================================================== */

__INTERRUPT
__HIGH_CODE
void FP_UART_IRQHandler(void)
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

void fp_uart_init(void)
{
    /* 1. TX pin (idle HIGH before enable to avoid framing glitch on host) */
    FP_TX_SETHIGH();
    FP_TX_MODECFG();

    /* 2. RX pin — pull-up */
    FP_RX_MODECFG();

    /* 3. UART default init (baud, FIFO on, 8N1) + override baud */
    U_FN(_DefInit)();
    U_FN(_BaudRateCfg)(FP_UART_BAUD);

    /* 4. FIFO trigger + enable interrupts: RECV_RDY + RX line status.
     *    TIMEOUT needs FIFO + at least 1 byte which triggers it implicitly. */
    U_FN(_ByteTrigCfg)(FP_UART_TRIG);

    /* Set priority below BLE interrupts, then enable the IRQ vector. */
    PFIC_SetPriority(FP_UART_IRQn, FP_UART_IRQ_PRI);
    U_FN(_INTCfg)(ENABLE, RB_IER_RECV_RDY | RB_IER_LINE_STAT);
    PFIC_EnableIRQ(FP_UART_IRQn);

    s_ready = TRUE;
}

/* =======================================================================
 * Drain — consume ring buffer into fp_proto_rx_byte().
 * Called from the task context.
 * ===================================================================== */

uint16_t fp_uart_drain(void)
{
    uint16_t n = 0;
    int c;
    while ((c = ring_pop()) >= 0) {
        fp_proto_rx_byte((uint8_t)c);
        n++;
    }
    return n;
}

uint16_t fp_uart_available(void)
{
    return ring_avail();
}

/* =======================================================================
 * TX side — blocking multi-byte out the FP UART.
 * Pushes a full protocol frame; no implicit CR/LF translation.
 * ===================================================================== */

void fp_uart_send(const uint8_t *buf, uint16_t len)
{
    // PRINT("FP_TX: ");
    if (!s_ready || buf == NULL || len == 0) return;
    while (len--) {
        /* Wait until TX FIFO is not full. */
        // PRINT("%02X ", *buf);
        while (U_R8(_TFC) == UART_FIFO_SIZE) { /* spin */ }
        U_SendByte(*buf++);
    }
    // PRINT("\n");
}
