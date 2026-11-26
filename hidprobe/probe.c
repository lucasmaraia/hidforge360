// hidprobe: read-only test plugin for Dashlaunch, built with xenon-forge.
//
// It proves that a system DLL from this toolchain loads on the console, and
// records what the HID driver port needs to know: the kernel build, the
// addresses of the exports the driver resolves by ordinal, and the code bytes
// at the fixed 17559 addresses the driver hooks. Nothing on the console is
// modified. The result is written to the root of the USB drive.
#include <xbox360/xboxkrnl.h>

#define LOG_CAP 6000
static char g_log[LOG_CAP];
static ULONG g_len;

static void put_char(char c) {
    if (g_len < LOG_CAP - 1) {
        g_log[g_len++] = c;
    }
}

static void put_str(const char *s) {
    while (*s) {
        put_char(*s++);
    }
}

static void put_hex(ULONG value) {
    static const char digits[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) {
        put_char(digits[(value >> shift) & 0xF]);
    }
}

static void put_dec(ULONG value) {
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) {
        put_char(tmp[--n]);
    }
}

static ULONG str_len(const char *s) {
    ULONG n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

// Writes the log buffer to `name` in the root of the first USB mass storage
// device that accepts it. Returns the status of the last attempt.
static NTSTATUS write_log(const char *name) {
    NTSTATUS status = -1;
    for (char unit = '0'; unit <= '2'; unit++) {
        char path[64];
        ULONG n = 0;
        const char *prefix = "\\Device\\Mass";
        while (*prefix) {
            path[n++] = *prefix++;
        }
        path[n++] = unit;
        path[n++] = '\\';
        for (const char *p = name; *p && n < sizeof(path) - 1; p++) {
            path[n++] = *p;
        }
        path[n] = 0;

        OBJECT_STRING object_name;
        object_name.Length = (USHORT)n;
        object_name.MaximumLength = (USHORT)(n + 1);
        object_name.Buffer = path;

        OBJECT_ATTRIBUTES attributes;
        attributes.RootDirectory = NULL;
        attributes.ObjectName = &object_name;
        attributes.Attributes = OBJ_CASE_INSENSITIVE;

        IO_STATUS_BLOCK io;
        HANDLE file = NULL;
        status = NtCreateFile(&file, GENERIC_WRITE | SYNCHRONIZE, &attributes, &io, NULL,
                              FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_SUPERSEDE,
                              FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
        if (!NT_SUCCESS(status)) {
            continue;
        }
        status = NtWriteFile(file, NULL, NULL, NULL, &io, g_log, g_len, NULL);
        NtClose(file);
        if (NT_SUCCESS(status)) {
            return status;
        }
    }
    return status;
}

static void put_version(void) {
    PXBOX_KRNL_VERSION v = &XboxKrnlVersion;
    put_str("kernel ");
    put_dec(v->Major);
    put_char('.');
    put_dec(v->Minor);
    put_char('.');
    put_dec(v->Build);
    put_char('.');
    put_dec(v->Qfe);
    put_str("\r\n");
}

static void put_words(ULONG address, int count) {
    for (int i = 0; i < count; i++) {
        ULONG a = address + (ULONG)i * 4;
        put_char(' ');
        if (MmIsAddressValid((PVOID)a)) {
            put_hex(*(volatile ULONG *)a);
        } else {
            put_str("--------");
        }
    }
}

struct ordinal_entry {
    const char *name;
    ULONG ordinal;
};

static const struct ordinal_entry kernel_ordinals[] = {
    {"UsbdGetDeviceDescriptor", 759},      {"UsbdGetEndpointDescriptor", 744},
    {"UsbdAddDeviceComplete", 740},        {"UsbdOpenDefaultEndpoint", 746},
    {"UsbdOpenEndpoint", 747},             {"UsbdGetDeviceSpeed", 742},
    {"UsbdQueueAsyncTransfer", 748},       {"UsbdQueueCloseEndpoint", 750},
    {"UsbdQueueCloseDefaultEndpoint", 749}, {"UsbdRemoveDeviceComplete", 751},
    {"MmFreePhysicalMemory", 189},         {"XInputdReadState", 486},
};

static const struct ordinal_entry xam_ordinals[] = {
    {"XamInputGetCapabilitiesEx", 685},
    {"XamInputSetState", 402},
    {"XNotify patch site", 1183},
    {"XNotifyQueueUI", 656},
};

struct fixed_entry {
    const char *name;
    ULONG address;
};

// Addresses the upstream driver hard-codes for 17559 retail.
static const struct fixed_entry fixed_addresses[] = {
    {"HidAddDevice", 0x800E4D68},
    {"HidRemoveDevice", 0x800E4D28},
    {"UsbdGetInterfaceDescriptor", 0x800D8500},
    {"XamUserBindDeviceCallback", 0x816D9060},
    {"UsbdPowerDownNotification", 0x800D8FC8},
    {"UsbdDriverEntry", 0x800D8D08},
    {"XNotify type limit", 0x816AB7A4},
};

static void put_ordinals(const char *module, const struct ordinal_entry *entries, int count) {
    PLDR_DATA_TABLE_ENTRY handle = NULL;
    NTSTATUS status = XexGetModuleHandle(module, &handle);
    put_str(module);
    put_str(" handle ");
    put_hex((ULONG)handle);
    put_str(" status ");
    put_hex((ULONG)status);
    put_str("\r\n");
    if (!NT_SUCCESS(status) || !handle) {
        return;
    }
    for (int i = 0; i < count; i++) {
        PVOID address = NULL;
        status = XexGetProcedureAddress(handle, entries[i].ordinal, &address);
        put_str("  ");
        put_dec(entries[i].ordinal);
        put_char(' ');
        put_str(entries[i].name);
        put_str(" = ");
        put_hex((ULONG)address);
        if (!NT_SUCCESS(status)) {
            put_str(" status ");
            put_hex((ULONG)status);
        } else {
            put_str(" :");
            put_words((ULONG)address, 4);
        }
        put_str("\r\n");
    }
}

static void probe_thread(PVOID context) {
    (void)context;

    // Let the other plugins and the dashboard finish loading first.
    LARGE_INTEGER interval;
    interval.QuadPart = -10LL * 10000000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);

    g_len = 0;
    put_str("hidprobe 1: thread running\r\n");
    put_version();
    put_str("process type ");
    put_dec((ULONG)KeGetCurrentProcessType());
    put_str("\r\n");

    put_ordinals("xboxkrnl.exe", kernel_ordinals,
                 (int)(sizeof(kernel_ordinals) / sizeof(kernel_ordinals[0])));
    put_ordinals("xam.xex", xam_ordinals, (int)(sizeof(xam_ordinals) / sizeof(xam_ordinals[0])));

    put_str("fixed 17559 addresses (8 words each)\r\n");
    for (ULONG i = 0; i < sizeof(fixed_addresses) / sizeof(fixed_addresses[0]); i++) {
        put_str("  ");
        put_hex(fixed_addresses[i].address);
        put_char(' ');
        put_str(fixed_addresses[i].name);
        put_str(" :");
        put_words(fixed_addresses[i].address, 8);
        put_str("\r\n");
    }
    put_str("end\r\n");

    NTSTATUS status = write_log("hidprobe-2-thread.log");
    DbgPrint("hidprobe: thread log status %08x, %u bytes\n", (ULONG)status, g_len);
}

BOOL DllMain(HANDLE Module, DWORD Reason, PVOID Reserved) {
    (void)Reserved;
    if (Reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }

    DbgPrint("hidprobe: loaded at %p\n", Module);

    g_len = 0;
    put_str("hidprobe 1: DllMain ran, module ");
    put_hex((ULONG)Module);
    put_str("\r\n");
    put_version();
    write_log("hidprobe-1-dllmain.log");

    HANDLE thread = NULL;
    NTSTATUS status = ExCreateThread(&thread, 0, NULL, NULL, probe_thread, NULL, EX_CREATE_FLAG_SYSTEM);
    if (NT_SUCCESS(status) && thread) {
        NtClose(thread);
    }
    return TRUE;
}
