// Controllers already plugged in at boot.
//
// The kernel enumerates them before this plugin hooks HidAddDevice, so they never
// reach the hook. Upstream restarts the whole USB stack to enumerate them again;
// that takes down the USB drive the system runs from, so it is out of this port.
//
// The narrower route (the hiddriver360 fork by kadirdogan, kernel 17511) walks the
// ports with kernel exports and resets only the nodes no driver claimed, so the
// kernel removes and re-enumerates that one device and the hook sees it:
//   758 UsbdResetDevice   880 UsbdGetRootHubDeviceNode
//   881 UsbdGetPortDeviceNode   886 KeCallAndWaitForDpcRoutine (processor 2)
//
// Read-only scan on the console (17559, logs\hidforge-xe2-console5.log): a node is
// 16 bytes, owner at +4, flags at +8, type at +11, and root[8] & 3 is the
// controller index, as on 17511. With the XE 2 plugged in at boot its port node
// was c3 p4: owner 0, flags 13, type 1c, the only one the fork's rule picks; the
// USB drive's node has a driver as owner and is never touched. The first bytes
// of the four exports are checked first, so another kernel does nothing.

#ifndef HIDFORGE_BOOTSCAN_RESET
#define HIDFORGE_BOOTSCAN_RESET 1
#endif

typedef void (*HfDpcRoutine)(void*, void*, void*, void*);
typedef NTSTATUS (*HfCallDpc)(HfDpcRoutine, void*, DWORD, void*, void*);
typedef BYTE* (*HfRootNode)(DWORD);
typedef BYTE* (*HfPortNode)(BYTE*, DWORD);
typedef void (*HfResetDevice)(BYTE*);

static HfCallDpc g_bootCallDpc;
static HfRootNode g_bootRootNode;
static HfPortNode g_bootPortNode;
static HfResetDevice g_bootReset;

struct HfBootNode {
	BYTE* node;
	BYTE valid;        // node readable
	BYTE eligible;     // no driver, single device: may be reset
	BYTE bytes[16];
};

static HfBootNode g_bootNodes[32];
static BYTE g_bootPortDone[32];        // port already reset once
#if HIDFORGE_DIAG
static BYTE g_bootLastValid[32];
static BYTE g_bootLastBytes[32][16];
#endif
static BYTE* g_bootRootPtr[4];
static volatile LONG g_bootDpcRan;
static BYTE* volatile g_bootResetNode;
static volatile LONG g_bootResetDone;

// First bytes of the exports on 17559, read on the console.
static const BYTE kBoot886[4] = { 0x7d, 0x88, 0x02, 0xa6 };
static const BYTE kBoot880[8] = { 0x3d, 0x60, 0x80, 0x1b, 0x54, 0x6a, 0x10, 0x3a };
static const BYTE kBoot881[8] = { 0x89, 0x23, 0x00, 0x08, 0x3d, 0x40, 0x80, 0x1b };
static const BYTE kBoot758[8] = { 0x38, 0x80, 0x00, 0x01, 0x4b, 0xff, 0xff, 0x34 };

// Not inlined, and every MmIsAddressValid result is tested right after the call:
// inlined, the compiler kept the combined result in cr2 across the next kernel
// call, and cr2-cr4 are not trusted after a kernel call (CONHECIMENTO.md).
static __attribute__((noinline)) int HfValid(const void* p) {
	return MmIsAddressValid((PVOID)p) ? 1 : 0;
}

static __attribute__((noinline)) bool HfReadable(const void* p, DWORD length) {
	if (!p)
		return false;
	// Integers, not branches: both results are plain register values when they meet.
	int first = HfValid(p);
	int last = HfValid((const BYTE*)p + length - 1);
	return (first & last) != 0;
}

// Compares without keeping a condition across the MmIsAddressValid calls: the
// compiler had put a flag in cr2 there, and cr2-cr4 are not trusted after a
// kernel call (CONHECIMENTO.md). The readability check is done by the caller.
static int HfCodeDiff(const void* code, const BYTE* expected, DWORD length) {
	const BYTE* b = (const BYTE*)code;
	int diff = 0;
	for (DWORD i = 0; i < length; i++)
		diff |= b[i] ^ expected[i];
	return diff;
}

// The fork's rule: no owner, no transitional/composite/hub bits, type 2 (no
// matched driver) or 4 (single-interface device).
static bool HfNodeEligible(const BYTE* node) {
	DWORD owner = *(const DWORD*)(node + 4);
	BYTE flags = node[8], type = node[11];
	return !owner && !(flags & 0xE0) && !(type & 0xE0) && ((type & 7) == 2 || (type & 7) == 4);
}

static bool HfNodeIsOurs(const BYTE* node) {
	for (int i = 0; i < 4; i++)
		if ((const BYTE*)connectedControllers[i].deviceHandle == node)
			return true;
	return false;
}

#if HIDFORGE_DIAG
static void HfLogBytes(const char* name, const BYTE* p, DWORD length) {
	for (DWORD off = 0; off < length; off += 16) {
		const BYTE* b = p + off;
		DbgPrint("HIDFORGE: boot %s +%02x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			name, off, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	}
}
#endif

// Runs at DPC level on processor 2, next to the kernel's own USB DPCs: reads
// only, keeps no pointer for later use except to find the node again.
static void HfBootScanDpc(void*, void*, void*, void*) {
	InterlockedExchange(&g_bootDpcRan, 1);
	for (DWORD c = 0; c < 4; c++) {
		BYTE* root = g_bootRootNode(c);
		g_bootRootPtr[c] = root;
		if (!HfReadable(root, 16) || (root[8] & 3) != c)
			continue;
		for (DWORD p = 0; p < 8; p++) {
			HfBootNode* rec = &g_bootNodes[c * 8 + p];
			rec->node = g_bootPortNode(root, p);
			if (!HfReadable(rec->node, 16))
				continue;
			rec->valid = 1;
			memcpy(rec->bytes, rec->node, 16);
			rec->eligible = HfNodeEligible(rec->node) && !HfNodeIsOurs(rec->node);
		}
	}
}

// Resets one node, after checking again that it is still unclaimed.
static void HfBootResetDpc(void*, void*, void*, void*) {
	BYTE* node = g_bootResetNode;
	if (HfReadable(node, 16) && HfNodeEligible(node) && !HfNodeIsOurs(node)) {
		g_bootReset(node);
		InterlockedExchange(&g_bootResetDone, 1);
	}
}

static void HfBootPause(LONG ms) {
	LARGE_INTEGER interval;
	interval.QuadPart = -(LONGLONG)ms * 10000LL;
	KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

static void HfBootScanThread(PVOID) {
	HfBootPause(1000); // first pass 1 s after load; later passes catch devices still enumerating

	void* kernel = ShimModuleHandle("xboxkrnl.exe");
	void *call = NULL, *root = NULL, *port = NULL, *reset = NULL;
	if (!kernel || !ShimGetProc(kernel, 886, &call) || !ShimGetProc(kernel, 880, &root) ||
		!ShimGetProc(kernel, 881, &port) || !ShimGetProc(kernel, 758, &reset)) {
		DbgPrint("HIDFORGE: boot scan: exports missing (886 %p, 880 %p, 881 %p, 758 %p)\n", call, root, port, reset);
		return;
	}
	DbgPrint("HIDFORGE: boot scan: KeCallAndWaitForDpcRoutine %p, UsbdGetRootHubDeviceNode %p, UsbdGetPortDeviceNode %p, UsbdResetDevice %p\n",
		call, root, port, reset);
	if (!HfReadable(call, 8) || !HfReadable(root, 8) || !HfReadable(port, 8) || !HfReadable(reset, 8)) {
		DbgPrint("HIDFORGE: boot scan: export code not readable, nothing done\n");
		return;
	}
	int diff = HfCodeDiff(call, kBoot886, sizeof(kBoot886)) | HfCodeDiff(root, kBoot880, sizeof(kBoot880)) |
		HfCodeDiff(port, kBoot881, sizeof(kBoot881)) | HfCodeDiff(reset, kBoot758, sizeof(kBoot758));
	if (diff) {
#if HIDFORGE_DIAG
		HfLogBytes("code886", (const BYTE*)call, 16);
		HfLogBytes("code880", (const BYTE*)root, 16);
		HfLogBytes("code881", (const BYTE*)port, 16);
		HfLogBytes("code758", (const BYTE*)reset, 16);
#endif
		DbgPrint("HIDFORGE: boot scan: kernel check failed, nothing done (replug controllers plugged in at boot)\n");
		return;
	}

	g_bootCallDpc = (HfCallDpc)call;
	g_bootRootNode = (HfRootNode)root;
	g_bootPortNode = (HfPortNode)port;
	g_bootReset = (HfResetDevice)reset;

	// Scans every 500 ms from 1 s to ~12 s after load. Resetting the port of a
	// Bluetooth adapter such as the XE 2 drops its link to the pad (console log
	// of round 5), so the reset has to happen early in the boot, before the pad
	// is switched on. Each port is reset at most once, so a device that still
	// has no driver afterwards is left alone; a node still being enumerated
	// (transitional bits) is simply looked at again on the next pass.
	int resets = 0;
	for (int pass = 0; pass < 23; pass++) {
		memset(g_bootNodes, 0, sizeof(g_bootNodes));
		InterlockedExchange(&g_bootDpcRan, 0);
		NTSTATUS status = g_bootCallDpc(HfBootScanDpc, NULL, 2, NULL, NULL);
		if (!NT_SUCCESS(status) || !g_bootDpcRan) {
			DbgPrint("HIDFORGE: boot scan pass %d: DPC status %08X, ran %d; stopping\n", pass, (ULONG)status, (int)g_bootDpcRan);
			break;
		}
		for (DWORD i = 0; i < 32; i++) {
			HfBootNode* rec = &g_bootNodes[i];
#if HIDFORGE_DIAG
			// Log a port only when what it holds changed since the last pass.
			if (rec->valid != g_bootLastValid[i] || (rec->valid && HfCodeDiff(rec->bytes, g_bootLastBytes[i], 16))) {
				if (rec->valid) {
					DbgPrint("HIDFORGE: boot pass %d node c%u p%u at %p owner %08x flags %02x type %02x%s\n", pass, i / 8, i % 8,
						rec->node, *(DWORD*)(rec->bytes + 4), rec->bytes[8], rec->bytes[11], rec->eligible ? " (no driver)" : "");
					char name[8] = { 'c', (char)('0' + i / 8), 'p', (char)('0' + i % 8), 0 };
					HfLogBytes(name, rec->bytes, 16);
				}
				else
					DbgPrint("HIDFORGE: boot pass %d node c%u p%u gone\n", pass, i / 8, i % 8);
				g_bootLastValid[i] = rec->valid;
				memcpy(g_bootLastBytes[i], rec->bytes, 16);
			}
#endif
			if (!rec->valid || !rec->eligible || g_bootPortDone[i] || !HIDFORGE_BOOTSCAN_RESET)
				continue;
			g_bootPortDone[i] = 1;
			g_bootResetNode = rec->node;
			InterlockedExchange(&g_bootResetDone, 0);
			status = g_bootCallDpc(HfBootResetDpc, NULL, 2, NULL, NULL);
			DbgPrint("HIDFORGE: boot reset pass %d t=%u c%u p%u node %p: DPC status %08X, reset %s\n", pass, GetTickCount(), i / 8, i % 8, rec->node,
				(ULONG)status, g_bootResetDone ? "sent" : "skipped (node changed)");
			if (g_bootResetDone)
				resets++;
			HfBootPause(1000); // one device initialising at a time
		}
		HfBootPause(500);
	}
	DbgPrint("HIDFORGE: boot scan: done, %d reset(s)%s\n", resets, HIDFORGE_BOOTSCAN_RESET ? "" : " (read-only build)");
}

static void BootScanStart() {
	HANDLE thread = NULL;
	NTSTATUS status = ExCreateThread(&thread, 0x8000, NULL, NULL, HfBootScanThread, NULL, EX_CREATE_FLAG_SYSTEM | 0x18000424);
	DbgPrint("HIDFORGE: boot scan thread status %08X\n", (ULONG)status);
	if (NT_SUCCESS(status) && thread)
		NtClose(thread);
}
