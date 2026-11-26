// Compatibility layer that lets the hiddriver360 sources build with the
// xenon-forge toolchain instead of the Microsoft SDK. It replaces <xtl.h>,
// <xkelib.h> and the parts of the C/C++ runtime the driver uses.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <xbox360/xboxkrnl.h>
#include <xbox360/xam.h>

typedef unsigned int UINT32;
typedef int INT32;
typedef short INT16;
typedef BYTE *LPBYTE;

#define _In_
#define _Out_
#define _Inout_
#define APIENTRY
#define CALLBACK

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS 0
#endif
#ifndef NT_ERROR
#define NT_ERROR(Status) ((((ULONG)(Status)) >> 30) == 3)
#endif
#ifndef ERROR_SUCCESS
#define ERROR_SUCCESS 0
#endif
#ifndef ERROR_INVALID_PARAMETER
#define ERROR_INVALID_PARAMETER 87
#endif
#ifndef ERROR_DEVICE_NOT_CONNECTED
#define ERROR_DEVICE_NOT_CONNECTED 1167
#endif

extern "C" {
void *memset(void *dst, int value, size_t size);
void *memcpy(void *dst, const void *src, size_t size);
int memcmp(const void *a, const void *b, size_t size);
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void free(void *pointer);
}

// Diagnostics: the log file on the USB drive, the heartbeat and the stage
// display on the ring of light. The final build sets this to 0 and none of it
// is compiled in, so the plugin writes no file and never touches the ring.
// A build for investigating something is -DHIDFORGE_DIAG=1.
#ifndef HIDFORGE_DIAG
#define HIDFORGE_DIAG 0
#endif

#if HIDFORGE_DIAG
// Formatted log line. Safe at any IRQL: it only appends to a memory buffer,
// which a system thread writes to hidforge.log on the USB drive.
void hid_log(const char *format, ...);
#undef DbgPrint
#define DbgPrint hid_log
#else
#undef DbgPrint
#define DbgPrint(...) ((void)0)
#endif

// Output transfer blocks, one per controller slot. They hold a USB transfer
// block and its payload, so they come from the kernel pool, which the USB
// stack can read for an outgoing transfer, and are allocated once at plugin
// load: allocating inside the HID add hook hangs this console.
#define SHIM_OUTPUT_BLOCKS     4
#define SHIM_OUTPUT_BLOCK_SIZE 128
extern void *g_shimOutputBlock[SHIM_OUTPUT_BLOCKS];

#if HIDFORGE_DIAG
// Last step the driver reached, shown on the ring of light by a thread that
// does no USB or file I/O.
extern volatile LONG g_stage;
static inline void Stage(LONG stage) {
    if (g_stage < stage) {
        g_stage = stage;
    }
}

// Writes pending log lines from the calling thread (passive level only).
void ShimPollLog();
#else
static inline void Stage(LONG) {}
static inline void ShimPollLog() {}
#endif

// Data cache write-back and instruction cache invalidation after patching code.
void ShimFlushCode(void *address, ULONG length);

// Shuts the console down. Called from a title thread at passive level, when
// the PS button has been held long enough.
void ShimPowerOff();

// Starts the log writer thread and resolves the imports the SDK headers lack.
bool ShimInit();

void *ShimModuleHandle(const char *name);
bool ShimGetProc(void *module, ULONG ordinal, void *out);
#define GetModuleHandleA(name) ShimModuleHandle(name)
#define XexGetProcedureAddress(module, ordinal, out) ShimGetProc((module), (ordinal), (void *)(out))

DWORD GetTickCount();
extern DWORD (*XamInputSendXenonButtonPress)(DWORD userIndex);

static inline LONG InterlockedExchange(volatile LONG *target, LONG value) {
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

static inline LONG InterlockedCompareExchange(volatile LONG *target, LONG exchange, LONG comparand) {
    return __sync_val_compare_and_swap(target, comparand, exchange);
}

static inline void __lwsync() {
    __asm__ volatile("lwsync");
}
