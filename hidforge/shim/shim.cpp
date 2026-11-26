#include "shim.h"

#include <stdarg.h>

// The shim macros redirect these names for the driver sources; the shim
// itself needs the real imports.
#undef XexGetProcedureAddress
#undef GetModuleHandleA

extern "C" {

void *memset(void *dst, int value, size_t size) {
    volatile unsigned char *d = (volatile unsigned char *)dst;
    while (size--) {
        *d++ = (unsigned char)value;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t size) {
    volatile unsigned char *d = (volatile unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (size--) {
        *d++ = *s++;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t size) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (; size--; x++, y++) {
        if (*x != *y) {
            return *x < *y ? -1 : 1;
        }
    }
    return 0;
}

// Pool memory, so the driver can allocate inside USB completion callbacks.
void *malloc(size_t size) {
    return ExAllocatePoolWithTag(size, 0x48494446); // 'HIDF'
}

void *calloc(size_t count, size_t size) {
    size_t total = count * size;
    void *pointer = malloc(total);
    if (pointer) {
        memset(pointer, 0, total);
    }
    return pointer;
}

void free(void *pointer) {
    if (pointer) {
        ExFreePool(pointer);
    }
}

// Static objects with destructors register them here. A system module is
// never unloaded in practice, so nothing is recorded.
void *__dso_handle = 0;
int __cxa_atexit(void (*)(void *), void *, void *) {
    return 0;
}
void __cxa_pure_virtual() {
    for (;;) {
    }
}

} // extern "C"

// ---------------------------------------------------------------------------
// Log (diagnostic builds only)
// ---------------------------------------------------------------------------

#if HIDFORGE_DIAG

#define LOG_CAPACITY (64 * 1024)
static char g_logBuffer[LOG_CAPACITY];
static volatile LONG g_logLength;
static volatile LONG g_logDirty;
// Bytes of the log that are on disk.
static volatile LONG g_logWritten;

// Waiting for the disk inside the USB context is only safe while chasing a
// hang, never in a build that is used normally.
#ifndef HIDFORGE_SYNC_LOG
#define HIDFORGE_SYNC_LOG 0
#endif

struct LineBuffer {
    char text[320];
    unsigned length;
};

static void line_put(LineBuffer *line, char c) {
    if (line->length < sizeof(line->text)) {
        line->text[line->length++] = c;
    }
}

static void line_number(LineBuffer *line, unsigned long value, unsigned base, bool upper, int width, char pad,
                        bool negative) {
    char digits[24];
    int n = 0;
    const char *alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        digits[n++] = alphabet[value % base];
        value /= base;
    } while (value && n < (int)sizeof(digits));
    int total = n + (negative ? 1 : 0);
    if (negative && pad == '0') {
        line_put(line, '-');
    }
    for (; total < width; total++) {
        line_put(line, pad);
    }
    if (negative && pad != '0') {
        line_put(line, '-');
    }
    while (n) {
        line_put(line, digits[--n]);
    }
}

void hid_log(const char *format, ...) {
    LineBuffer line;
    line.length = 0;

    va_list args;
    va_start(args, format);
    for (const char *p = format; *p; p++) {
        if (*p != '%') {
            if (*p == '\n' && (p == format || p[-1] != '\r')) {
                line_put(&line, '\r');
            }
            line_put(&line, *p);
            continue;
        }
        p++;
        char pad = ' ';
        int width = 0;
        if (*p == '0') {
            pad = '0';
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p++ - '0');
        }
        while (*p == 'l' || *p == 'h') {
            p++;
        }
        switch (*p) {
        case 'd':
        case 'i': {
            long value = va_arg(args, long);
            bool negative = value < 0;
            line_number(&line, negative ? (unsigned long)(-value) : (unsigned long)value, 10, false, width, pad,
                        negative);
            break;
        }
        case 'u':
            line_number(&line, va_arg(args, unsigned long), 10, false, width, pad, false);
            break;
        case 'x':
            line_number(&line, va_arg(args, unsigned long), 16, false, width, pad, false);
            break;
        case 'X':
            line_number(&line, va_arg(args, unsigned long), 16, true, width, pad, false);
            break;
        case 'p':
            line_number(&line, va_arg(args, unsigned long), 16, true, 8, '0', false);
            break;
        case 's': {
            const char *text = va_arg(args, const char *);
            for (text = text ? text : "(null)"; *text; text++) {
                line_put(&line, *text);
            }
            break;
        }
        case 'c':
            line_put(&line, (char)va_arg(args, int));
            break;
        case '%':
            line_put(&line, '%');
            break;
        case 0:
            p--;
            break;
        default:
            line_put(&line, '?');
            break;
        }
    }
    va_end(args);

    // Reserve the range first so concurrent callers never overlap.
    LONG start = __atomic_fetch_add(&g_logLength, (LONG)line.length, __ATOMIC_SEQ_CST);
    if (start < 0 || start + (LONG)line.length > LOG_CAPACITY) {
        __atomic_fetch_sub(&g_logLength, (LONG)line.length, __ATOMIC_SEQ_CST);
        return;
    }
    memcpy(&g_logBuffer[start], line.text, line.length);
    InterlockedExchange(&g_logDirty, 1);

#if HIDFORGE_SYNC_LOG
    // Debug builds: give the writer thread up to 400 ms to put this line on
    // disk before the caller goes on, so the last line before a crash is not
    // lost. The writer runs on another hardware thread.
    LONG end = start + (LONG)line.length;
    for (int i = 0; i < 400 && g_logWritten < end; i++) {
        KeStallExecutionProcessor(1000);
    }
#endif
}

static NTSTATUS write_log_file() {
    NTSTATUS status = -1;
    LONG length = g_logLength;
    if (length > LOG_CAPACITY) {
        length = LOG_CAPACITY;
    }
    for (char unit = '0'; unit <= '2'; unit++) {
        char path[] = "\\Device\\Mass0\\hidforge.log";
        path[12] = unit;

        OBJECT_STRING name;
        name.Length = (USHORT)(sizeof(path) - 1);
        name.MaximumLength = (USHORT)sizeof(path);
        name.Buffer = path;

        OBJECT_ATTRIBUTES attributes;
        attributes.RootDirectory = NULL;
        attributes.ObjectName = &name;
        attributes.Attributes = OBJ_CASE_INSENSITIVE;

        IO_STATUS_BLOCK io;
        HANDLE file = NULL;
        status = NtCreateFile(&file, GENERIC_WRITE | SYNCHRONIZE, &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL,
                              FILE_SHARE_READ, FILE_SUPERSEDE,
                              FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
        if (!NT_SUCCESS(status)) {
            continue;
        }
        status = NtWriteFile(file, NULL, NULL, NULL, &io, g_logBuffer, (ULONG)length, NULL);
        NtClose(file);
        if (NT_SUCCESS(status)) {
            InterlockedExchange(&g_logWritten, length);
            break;
        }
    }
    return status;
}

static volatile LONG g_logWriting;

// Writes the log if it has new lines. Only one caller writes at a time.
static void flush_log() {
    if (!g_logDirty || InterlockedCompareExchange(&g_logWriting, 1, 0) != 0) {
        return;
    }
    InterlockedExchange(&g_logDirty, 0);
    write_log_file();
    InterlockedExchange(&g_logWriting, 0);
}

// Second way to get the log on disk, for when the writer thread is gone: the
// driver calls this from its XamInputGetState hook, which runs on a thread of
// the current title at passive level.
void ShimPollLog() {
    static volatile LONG lastPoll;
    LONG now = (LONG)GetTickCount();
    if (now - lastPoll < 200) {
        return;
    }
    lastPoll = now;
    flush_log();
}

static void log_thread(PVOID) {
    DWORD started = GetTickCount();
    DWORD lastBeat = started;
    for (;;) {
        LARGE_INTEGER interval;
        interval.QuadPart = -250LL * 10000LL; // 250 ms
        KeDelayExecutionThread(KernelMode, FALSE, &interval);

        // Shows in the log whether this thread survives the switch to the dashboard.
        DWORD now = GetTickCount();
        if (now - started < 180000 && now - lastBeat >= 5000) {
            lastBeat = now;
            hid_log("HIDFORGE: writer thread alive, %u s\n", (now - started) / 1000);
        }
        flush_log();
    }
}

#endif // HIDFORGE_DIAG

// ---------------------------------------------------------------------------
// Imports and helpers
// ---------------------------------------------------------------------------

DWORD (*XamInputSendXenonButtonPress)(DWORD userIndex) = 0;

void *ShimModuleHandle(const char *name) {
    PLDR_DATA_TABLE_ENTRY handle = NULL;
    if (!NT_SUCCESS(XexGetModuleHandle(name, &handle))) {
        return NULL;
    }
    return handle;
}

bool ShimGetProc(void *module, ULONG ordinal, void *out) {
    PVOID address = NULL;
    NTSTATUS status = XexGetProcedureAddress((PLDR_DATA_TABLE_ENTRY)module, ordinal, &address);
    *(PVOID *)out = NT_SUCCESS(status) ? address : NULL;
    return NT_SUCCESS(status) && address;
}

DWORD GetTickCount() {
    // 100 ns units since boot, converted to milliseconds.
    return (DWORD)(KeTimeStampBundle.InterruptTime / 10000ULL);
}

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------

// The shutdown the dashboard performs when the guide menu's power option is
// chosen. Stock code continues after the call, so nothing follows it here.
void ShimPowerOff() {
    HalReturnToFirmware(HalPowerDownRoutine);
}

// ---------------------------------------------------------------------------
// Stage display on the ring of light (diagnostic builds only)
// ---------------------------------------------------------------------------

#if HIDFORGE_DIAG

// Last step the driver reached; written by the hooks with plain stores.
volatile LONG g_stage = 0;

static void ring(bool on) {
    SMC_RING_OF_LIGHT_REQUEST request;
    memset(&request, 0, sizeof(request));
    request.Command = SMC_COMMAND_SET_RING_OF_LIGHT;
    request.Flags = SMC_RING_OF_LIGHT_OVERRIDE;
    request.Quadrants = on ? SMC_RING_OF_LIGHT_QUADRANT_MASK : 0;
    HalSendSMCMessage((PSMC_MESSAGE)&request, NULL);
}

static void pause_ms(LONG ms) {
    LARGE_INTEGER interval;
    interval.QuadPart = -(LONGLONG)ms * 10000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

// Shows g_stage without any USB or file I/O, so it keeps working when the
// USB stack is stuck. Stage 0: one short blink every 3 s. Stages 1 to 14:
// that many blinks, then a 2 s pause. Stage 15 (controller in use): ring
// steady on.
static void led_thread(PVOID) {
    for (;;) {
        LONG stage = g_stage;
        if (stage >= 15) {
            ring(true);
            pause_ms(500);
            continue;
        }
        if (stage == 0) {
            ring(true);
            pause_ms(120);
            ring(false);
            pause_ms(2880);
            continue;
        }
        for (LONG i = 0; i < stage; i++) {
            ring(true);
            pause_ms(300);
            ring(false);
            pause_ms(300);
        }
        pause_ms(2000);
    }
}

#endif // HIDFORGE_DIAG

// Makes freshly written instructions visible to every processor.
void ShimFlushCode(void *address, ULONG length) {
    KeFlushCacheRange(address, length);
    KeSweepIcacheRange(address, length);
}

void *g_shimOutputBlock[SHIM_OUTPUT_BLOCKS];

bool ShimInit() {
    for (int i = 0; i < SHIM_OUTPUT_BLOCKS; i++) {
        g_shimOutputBlock[i] = calloc(1, SHIM_OUTPUT_BLOCK_SIZE);
        if (!g_shimOutputBlock[i]) {
            DbgPrint("HIDFORGE: no pool for output block %d\n", i);
        }
    }
    DbgPrint("HIDFORGE: output blocks at %p %p %p %p\n", g_shimOutputBlock[0], g_shimOutputBlock[1],
             g_shimOutputBlock[2], g_shimOutputBlock[3]);

#if HIDFORGE_DIAG
    HANDLE led = NULL;
    if (NT_SUCCESS(ExCreateThread(&led, 0x8000, NULL, NULL, led_thread, NULL, EX_CREATE_FLAG_SYSTEM | 0x18000424)) && led) {
        NtClose(led);
    }
#endif

    void *xam = ShimModuleHandle("xam.xex");
    if (!xam || !ShimGetProc(xam, 1286, &XamInputSendXenonButtonPress)) {
        DbgPrint("HIDFORGE: XamInputSendXenonButtonPress not found\n");
        return false;
    }

#if HIDFORGE_DIAG
    HANDLE thread = NULL;
    // Same flags as the thread the upstream driver creates, which survives
    // title switches: system thread, hardware threads 3 and 4, not debuggable.
    NTSTATUS status = ExCreateThread(&thread, 0x10000, NULL, NULL, log_thread, NULL, EX_CREATE_FLAG_SYSTEM | 0x18000424);
    DbgPrint("HIDFORGE: writer thread status %08X\n", (ULONG)status);
    if (NT_SUCCESS(status) && thread) {
        NtClose(thread);
    }
#endif
    return true;
}
