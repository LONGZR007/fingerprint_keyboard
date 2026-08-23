/* Minimal picolibc OS compat layer for CH583.
 *
 * Picolibc tinystdio (see <stdio.h>) exposes `struct __file` with a
 * put/get/flush vtable.  stdin/stdout/stderr are declared as
 * `FILE *const` (pointer to FILE, not FILE by value).  To make
 * printf/putchar/puts link and actually emit characters over the
 * SDK's debug UART we:
 *   1. Provide 3 `struct __file` objects whose .put callback routes
 *      to the SDK `_write(int fd, char *buf, int size)` syscall.
 *   2. Export `stdin`/`stdout`/`stderr` as pointer variables that
 *      point at those objects (picolic expects `FILE *const`).
 *
 * _sbrk() is already defined in StdPeriphDriver/CH58x_sys.c so we
 * intentionally do NOT duplicate it here.  The remaining syscall
 * stubs (_close/_fstat/...) are weak-ish placeholders only.
 */
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* Provided by the SDK (CH58x_sys.c). */
extern int _write(int fd, char *buf, int size);

/* ---- struct __file layout MUST match picolibc <stdio.h> ---- */
#if __PICOLIBC_UNGETC_SIZE == 2
typedef uint16_t __ungetc_t;
#else
typedef uint8_t  __ungetc_t;
#endif

struct cli__file {
    __ungetc_t unget;
    uint8_t    flags;
#define CLI___SRD   0x01
#define CLI___SWR   0x02
    int (*put)(char, struct cli__file *);
    int (*get)(struct cli__file *);
    int (*flush)(struct cli__file *);
};

/* Map FILE* → fd via a small cookie stored right after struct __file.
 * Simpler: since we only route stdout/stderr→fd=1,2 and never buffer,
 * we can give stdout and stderr the same put() callback.
 */
static int cli_stdout_put(char c, struct cli__file *f) {
    (void)f;
    char ch = c;
    _write(1, &ch, 1);
    return (unsigned char)ch;
}

static int cli_stderr_put(char c, struct cli__file *f) {
    (void)f;
    char ch = c;
    _write(2, &ch, 1);
    return (unsigned char)ch;
}

static int cli_stdin_get(struct cli__file *f) {
    (void)f;
    return -2;                          /* _FDEV_EOF — no blocking reader here */
}

static int cli_no_flush(struct cli__file *f) { (void)f; return 0; }

static struct cli__file s_stdout_obj = {
    .flags = CLI___SWR,
    .put   = cli_stdout_put,
    .get   = 0,
    .flush = cli_no_flush,
};
static struct cli__file s_stderr_obj = {
    .flags = CLI___SWR,
    .put   = cli_stderr_put,
    .get   = 0,
    .flush = cli_no_flush,
};
static struct cli__file s_stdin_obj = {
    .flags = CLI___SRD,
    .put   = 0,
    .get   = cli_stdin_get,
    .flush = cli_no_flush,
};

/* `FILE *const stdin / stdout / stderr`.  Declare as C variables so
 * the compiler emits a real data symbol (pointer) rather than trying
 * to alias a FILE.  Using `const` on the pointer value itself
 * matches the header's `FILE *const`. */
struct cli__file *const stdin  = &s_stdin_obj;
struct cli__file *const stdout = &s_stdout_obj;
struct cli__file *const stderr = &s_stderr_obj;

/* ---------- minimal syscall stubs ----------
 * Note: _sbrk lives in StdPeriphDriver/CH58x_sys.c.
 *       _write lives in StdPeriphDriver/CH58x_sys.c too.
 */
int _close(int fd)                      { (void)fd; return -1; }
int _fstat(int fd, void *st)            { (void)fd; (void)st; return -1; }
int _isatty(int fd)                     { return (fd >= 0 && fd <= 2) ? 1 : 0; }
long _lseek(int fd, long off, int w)    { (void)fd; (void)off; (void)w; return -1; }
int _read(int fd, void *buf, size_t len){ (void)fd; (void)buf; (void)len; return 0; }
int _kill(int pid, int sig)             { (void)pid; (void)sig; return -1; }
int _getpid(void)                       { return 1; }
void _exit(int s)                       { (void)s; for(;;){ __asm volatile("nop"); } }
