"""Turns the hiddriver360 v0.5 sources into sources that build with xenon-forge.

usage: port_v05.py <upstream hiddriver dir> <shim dir> <output dir>

v0.5 is the last release that is known to work on other 17559 consoles. It
handles only the DualShock 4 and DualSense, with fixed report layouts. Every
change to the upstream text is a named step; a step that no longer matches
stops the script.
"""
import os
import re
import shutil
import sys

upstream, shim, out = sys.argv[1:4]


def read(name):
    with open(os.path.join(upstream, name), encoding='latin-1', newline='') as f:
        return f.read().replace('\r\n', '\n')


def write(name, text):
    path = os.path.join(out, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='latin-1', newline='\n') as f:
        f.write(text)


def sub(text, pattern, replacement, what, count=1, flags=re.S):
    new, n = re.subn(pattern, lambda m: replacement, text, flags=flags)
    if n != count:
        raise SystemExit('step "%s": expected %d match(es), found %d' % (what, count, n))
    return new


def cut(text, start, end, replacement, what):
    """Replace from the first `start` up to, not including, the next `end`."""
    a = text.find(start)
    b = text.find(end, a + 1) if a >= 0 else -1
    if a < 0 or b < 0:
        raise SystemExit('step "%s": markers not found' % what)
    return text[:a] + replacement + text[b:]


t = read('main.cpp')

t = cut(t, '#include <xtl.h>', 'Detour HidAddDeviceDetour;', '#include "shim.h"\n#include "Detours.h"\n', 'includes')

t = sub(t, r'struct __declspec\(align\(2\)\) HidControllerExtension',
        'struct __attribute__((aligned(2))) HidControllerExtension', 'align(2)')
t = sub(t, r'\} __declspec\(align\(4\)\);', '} __attribute__((aligned(4)));', 'align(4)')
t = t.replace('unsigned __int8', 'uint8_t')
t = t.replace('XboxKrnlVersion->', 'XboxKrnlVersion.')
t = sub(t, r'HalSendSMCMessage\(Input, Output\);',
        'HalSendSMCMessage((PSMC_MESSAGE)Input, (PSMC_MESSAGE)Output);', 'SMC message types')

# No C++ runtime: the extension is plain data, zeroed like `new T()` does.
t = sub(t, r'new HidControllerExtension\(\)',
        '(HidControllerExtension*)calloc(1, sizeof(HidControllerExtension))', 'new extension')

# Names the driver uses for its own pointers but the SDK headers declare.
for name in ('MmFreePhysicalMemory', 'XamInputSetState', 'XamInputGetState'):
    t, n = re.subn(r'\b%s\b' % name, 'g_' + name, t)
    if not n:
        raise SystemExit('rename %s: no match' % name)

# Functions that fall off their end. The Microsoft compiler returns whatever
# is in r3; clang treats the path as unreachable and may emit no return at
# all, so they need an explicit one.
t = sub(t, r'(DbgPrint\("EINTIM: Removed virtual controller from XAM\.\\n"\);\n\t\treturn 0;\n\t\})\n\}',
        'DbgPrint("EINTIM: Removed virtual controller from XAM.\\n");\n\t\treturn 0;\n\t}\n\treturn 0;\n}',
        'HidRemoveDeviceHook return')
t = sub(t, r'(capabilities->Vibration\.wRightMotorSpeed = 0;\n\t\treturn ERROR_SUCCESS;\n\t\})\n\}',
        'capabilities->Vibration.wRightMotorSpeed = 0;\n\t\treturn ERROR_SUCCESS;\n\t}\n\treturn status;\n}',
        'XamInputGetCapabilitiesExHook return')

# 17559 retail: never reset the USB stack (it drops the USB drive the
# dashboard runs from), so the kernel patches that make a second
# UsbdDriverEntry call safe are not applied either.
t = cut(t, '\t\t//Remove two usb related bugchecks to allow reinitialisation of the usb driver\n\t\t*(DWORD*)0x800E05E4',
        '\t}\n\n\treturn true;', '', 'retail kernel patches')
t = cut(t, '\t\tDbgPrint("EINTIM: Resetting USB driver!\\n");', '\t\tDbgPrint("EINTIM: Hooks installed\\n");',
        '\t\tDbgPrint("HIDFORGE: USB stack left running; connect controllers after the plugin loads.\\n");\n',
        'USB reset')

t = sub(t, r'BOOL APIENTRY DllMain\(HANDLE Handle, DWORD Reason, PVOID Reserved\)\s*\{\s*if \(Reason == DLL_PROCESS_ATTACH\)\s*\{\n',
        'extern "C" BOOL DllMain(HANDLE Handle, DWORD Reason, PVOID Reserved)\n{\n'
        '\tif (Reason == DLL_PROCESS_ATTACH)\n\t{\n'
        '\t\tif (!ShimInit())\n\t\t\treturn FALSE;\n'
        '\t\tDbgPrint("HIDFORGE: v0.5 port, build " __DATE__ " " __TIME__ ", module %p\\n", Handle);\n',
        'DllMain entry')

# No pool allocation inside the hook: one static extension and report buffer
# per slot.
t = sub(t, r'int reportData = 0;\n',
        'static HidControllerExtension g_extStorage[4];\nstatic BYTE g_reportStorage[4][512];\nint reportData = 0;\n',
        'static storage')
t = sub(t, r'\(HidControllerExtension\*\)calloc\(1, sizeof\(HidControllerExtension\)\)',
        '(HidControllerExtension*)memset(&g_extStorage[index], 0, sizeof(HidControllerExtension))',
        'static extension')
t = sub(t, r'c\.reportData = malloc\(\(swap_endianness_16\(endpoint_descriptor->wMaxPacketSize\) & 0x7FF\) \* 2\);',
        'c.reportData = g_reportStorage[index];', 'static report buffer')
t = sub(t, r'memset\(c\.reportData, 0, \(swap_endianness_16\(endpoint_descriptor->wMaxPacketSize\) & 0x7FF\) \* 2\);',
        'memset(c.reportData, 0, sizeof(g_reportStorage[index]));', 'report buffer clear')
t = sub(t, r'[ \t]*free\(connectedControllers\[index\]\.reportData\);\n', '', 'no free')

# Stage markers, shown on the ring of light.
def mark(text, anchor, stage, what, after=False):
    if text.count(anchor) != 1:
        raise SystemExit('stage "%s": anchor found %d times' % (what, text.count(anchor)))
    call = 'Stage(%d);' % stage
    return text.replace(anchor, (anchor + ' ' + call) if after else (call + ' ' + anchor))

t = mark(t, 'DbgPrint("EINTIM: HID add device %p\\n", deviceHandle);', 1, 'hook entry')
t = mark(t, 'uint16_t vendorId = swap_endianness_16(device_descriptor->idVendor);', 2, 'descriptors')
t = mark(t, 'ControllerType controllerType = UNKNOWN_DEVICE;', 3, 'logged')
t = mark(t, 'DbgPrint("EINTIM: Controller detected. Initialising custom handler.\\n");', 4, 'detected', after=True)
t = mark(t, 'UsbdAddDeviceComplete(deviceHandle, 0);', 5, 'before add complete')
t = mark(t, 'NTSTATUS status = UsbdOpenDefaultEndpoint(', 6, 'before default endpoint')
t = mark(t, 'status = UsbdOpenEndpoint(deviceHandle, 3,', 7, 'before interrupt endpoint')
t = mark(t, 'controllerDriver->dwordC = controllerDriver->interruptEndpoint;', 8, 'endpoints open')
t = mark(t, 'uint8_t userIndex = -1;', 9, 'before bind')
t = mark(t, 'c.userIndex = userIndex;', 10, 'bound')
t = sub(t, r'Stage\(10\); c\.userIndex = userIndex;',
        'Stage(10); c.userIndex = userIndex; RumbleSetPlayer(index, userIndex);', 'DS3 player LED')
t = sub(t, r'return UsbdQueueAsyncTransfer\(deviceHandle, &controllerDriver->interruptEndpoint\);',
        '{ Stage(11); int queued = UsbdQueueAsyncTransfer(deviceHandle, &controllerDriver->interruptEndpoint); Stage(12); return queued; }',
        'queue first transfer')
t = sub(t, r'(int interruptHandler\(DWORD deviceHandle, int32_t a2\) \{\n)',
        'int interruptHandler(DWORD deviceHandle, int32_t a2) {\n\tStage(13);\n', 'interrupt entry')
t = sub(t, r'return UsbdQueueAsyncTransfer\(driverExtension->deviceHandle, &driverExtension->interruptEndpoint\);',
        '{ Stage(14); return UsbdQueueAsyncTransfer(driverExtension->deviceHandle, &driverExtension->interruptEndpoint); }',
        'interrupt requeue')
t = mark(t, '// HUD is open.', 15, 'state served')

# Rumble (shim/rumble.inc).
t = sub(t, r'Controller connectedControllers\[4\];\n',
        'Controller connectedControllers[4];\n\n#include "rumble.inc"\n', 'rumble include')
t = sub(t, r'Stage\(9\); uint8_t userIndex = -1;',
        'RumbleAttach(index, deviceHandle, controllerType);\n\t\tStage(9); uint8_t userIndex = -1;', 'rumble attach')
t = sub(t, r'(\t\tdeviceHandle2->driver->cleanedUpDone = 1;\n)',
        '\t\tdeviceHandle2->driver->cleanedUpDone = 1;\n\t\tRumbleDetach(index);\n', 'rumble detach')
t = sub(t, r'\{ Stage\(14\); return UsbdQueueAsyncTransfer',
        '{ Stage(14); if (index >= 0) RumblePump(index); return UsbdQueueAsyncTransfer', 'rumble pump')
t = sub(t, r'(\t\tif \(!c\)\n\t\t\treturn status;\n\n\t\treturn ERROR_SUCCESS;\n)',
        '\t\tif (!c)\n\t\t\treturn status;\n\n'
        '\t\tRumbleRequest((int)(c - connectedControllers), (const XINPUT_VIBRATION*)pInputState);\n'
        '\t\treturn ERROR_SUCCESS;\n', 'rumble request')
t = sub(t, r'capabilities->Vibration\.wLeftMotorSpeed = 0;\n\t\tcapabilities->Vibration\.wRightMotorSpeed = 0;',
        'capabilities->Vibration.wLeftMotorSpeed = g_rumble[c - connectedControllers].open ? 0xFFFF : 0;\n'
        '\t\tcapabilities->Vibration.wRightMotorSpeed = g_rumble[c - connectedControllers].open ? 0xFFFF : 0;',
        'rumble capabilities')

# DualShock 3 (shim/ds3.inc): detection, enable command, report layout.
t = sub(t, r'\tSONY_DUALSHOCK4,\n\tSONY_DUALSENSE,\n\};',
        '\tSONY_DUALSHOCK4,\n\tSONY_DUALSENSE,\n\tSONY_DUALSHOCK3,\n};', 'DS3 type')
t = sub(t, r'#include "rumble\.inc"\n', '#include "rumble.inc"\n#include "ds3.inc"\n', 'DS3 include')
t = sub(t, r'(\t\t\tcontrollerType = SONY_DUALSHOCK4;\n)(\t\}\n)',
        '\t\t\tcontrollerType = SONY_DUALSHOCK4;\n'
        '\t\tif (productId == DUALSHOCK3_PRODUCT_ID)\n\t\t\tcontrollerType = SONY_DUALSHOCK3;\n\t}\n', 'DS3 detection')
# Brook Wingman Converter XE 2 (0c12:0efa) in its PS4/PC mode. Measured on the PC
# (tools/teste-xe2.ps1): one HID interface (0), endpoints 0x84 IN and 0x03 OUT,
# input report 0x01 byte for byte the DualShock 4 USB one, output report 0x05 of
# 32 bytes accepted on the output endpoint only (SET_REPORT on the control
# endpoint fails), as on a real DS4. So it is driven as a DualShock 4.
t = sub(t, r'(const uint16_t DUALSENSE_EDGE_PRODUCT_ID = 0x0DF2;\n)',
        'const uint16_t DUALSENSE_EDGE_PRODUCT_ID = 0x0DF2;\n'
        'const uint16_t BROOK_VENDOR_ID = 0x0C12;\n'
        'const uint16_t BROOK_WINGMAN_XE2_PRODUCT_ID = 0x0EFA;\n', 'Brook IDs')
t = sub(t, r'(\t\tif \(productId == DUALSHOCK3_PRODUCT_ID\)\n\t\t\tcontrollerType = SONY_DUALSHOCK3;\n\t\}\n)',
        '\t\tif (productId == DUALSHOCK3_PRODUCT_ID)\n\t\t\tcontrollerType = SONY_DUALSHOCK3;\n\t}\n'
        '\tif (vendorId == BROOK_VENDOR_ID && productId == BROOK_WINGMAN_XE2_PRODUCT_ID)\n'
        '\t\tcontrollerType = SONY_DUALSHOCK4;\n', 'Brook XE 2 detection')

# Diagnostic builds log the first input report of each slot, so a converter
# that picks another mode in front of the console shows its real format.
t = sub(t, r'(\tif \(report->reportId == 1\) \{\n\t\tButtonsReport buttonReport = ButtonsReport\(\);\n)',
        '#if HIDFORGE_DIAG\n'
        '\t{\n'
        '\t\tstatic bool firstReportLogged[4];\n'
        '\t\tif (index >= 0 && !firstReportLogged[index]) {\n'
        '\t\t\tconst uint8_t* r = (const uint8_t*)report;\n'
        '\t\t\tfirstReportLogged[index] = true;\n'
        '\t\t\tDbgPrint("HIDFORGE: slot %d type %d first report %02x %02x %02x %02x %02x %02x %02x %02x '
        '%02x %02x %02x %02x %02x %02x %02x %02x\\n", index, (int)connectedControllers[index].controllerType,\n'
        '\t\t\t\tr[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);\n'
        '\t\t}\n'
        '\t}\n'
        '#endif\n'
        '\tif (report->reportId == 1) {\n\t\tButtonsReport buttonReport = ButtonsReport();\n', 'first report log')

# Upstream goes on with index -1 when the transfer belongs to no registered
# slot (a report racing the registration or the removal) and reads and writes
# connectedControllers[-1]. Such a report is dropped and the transfer re-queued.
t = sub(t, r'(#if HIDFORGE_DIAG\n\t\{\n\t\tstatic bool firstReportLogged\[4\];\n)',
        '\tif (index < 0)\n'
        '\t\treturn UsbdQueueAsyncTransfer(driverExtension->deviceHandle, &driverExtension->interruptEndpoint);\n\n'
        '#if HIDFORGE_DIAG\n\t{\n\t\tstatic bool firstReportLogged[4];\n', 'unknown slot guard')

t = sub(t, r'(\t\tswitch \(connectedControllers\[index\]\.controllerType\) \{\n)',
        '\t\tswitch (connectedControllers[index].controllerType) {\n'
        '\t\tcase SONY_DUALSHOCK3:\n\t\t\tDS3Fill((const uint8_t*)report, &buttonReport);\n\t\t\tbreak;\n',
        'DS3 report')
t = sub(t, r'RumbleAttach\(index, deviceHandle, controllerType\);\n',
        'if (controllerType == SONY_DUALSHOCK3) {\n\t\t\tRumbleAttachDS3(index, deviceHandle, controllerDriver);\n'
        '\t\t\tDS3Enable(index, controllerDriver);\n\t\t}\n'
        '\t\telse\n\t\t\tRumbleAttach(index, deviceHandle, controllerDriver, controllerType);\n', 'DS3 enable')

# PS button. Upstream taps the guide on every poll while the button is down,
# throttled to once a second, so holding it opens and closes the guide. Here it
# is one guide press per press, on the way down; holding does nothing more. (An
# earlier version shut the console down on a 2 s hold; the user did not want it.)
PS_OLD = """\t\tif (b.ps) {
\t\t\tDWORD now = GetTickCount();
\t\t\tif (now - lastPressTime >= cooldownDuration) {
\t\t\t\tlastPressTime = now;
\t\t\t\tXamInputSendXenonButtonPress(user);
\t\t\t}
\t\t}
"""

#
# Two guards, after round 6 on the console: a DualSense switched on during the
# boot logo stayed connected but dead in Aurora until switched on again. The
# pad is switched on with the PS button, so its first reports carry PS down;
# a guide press sent to XAM that early leaves the input routed to a system app
# that never shows, and the hook then hands titles an empty state.
#   - a PS already down when the slot is assigned only arms the button
#     (g_psWasDown starts true, and HidAddDeviceHook sets it again);
#   - no guide press in the first 20 s after boot.
PS_NEW = """\t\t{
\t\t\tint slot = (int)(c - connectedControllers);
\t\t\tif (b.ps && !g_psWasDown[slot]) {
\t\t\t\tDWORD now = GetTickCount();
\t\t\t\tif (now < HF_PS_BOOT_QUIET_MS)
\t\t\t\t\tDbgPrint("HIDFORGE: PS ignored (boot) slot %d t=%u\\n", slot, now);
\t\t\t\telse if (now - lastPressTime >= cooldownDuration) {
\t\t\t\t\tlastPressTime = now;
\t\t\t\t\tDbgPrint("HIDFORGE: PS guide sent slot %d user %u t=%u\\n", slot, user, now);
\t\t\t\t\tXamInputSendXenonButtonPress(user);
\t\t\t\t}
\t\t\t}
\t\t\tg_psWasDown[slot] = b.ps;
\t\t}
"""

t = sub(t, re.escape(PS_OLD), PS_NEW, 'PS button tap')
t = sub(t, r'(\t\t\t\tDbgPrint\("Assigning controller to index %d\\n", i\);\n\t\t\t\tindex = i;\n)',
        '\t\t\t\tDbgPrint("Assigning controller to index %d\\n", i);\n\t\t\t\tindex = i;\n'
        '\t\t\t\tg_psWasDown[i] = true; // a PS held while the pad connects only arms the button\n',
        'PS reset on add')
t = sub(t, r'(\t\tif \(XampInputRoutedToSysapp\[c->userIndex\]\) \{\n)',
        '#if HIDFORGE_DIAG\n'
        '\t\t{\n'
        '\t\t\tstatic DWORD lastRouted[4] = { 0, 0, 0, 0 };\n'
        '\t\t\tDWORD u = c->userIndex & 3;\n'
        '\t\t\tDWORD routed = XampInputRoutedToSysapp[c->userIndex];\n'
        '\t\t\tif (routed != lastRouted[u]) {\n'
        '\t\t\t\tlastRouted[u] = routed;\n'
        '\t\t\t\tDbgPrint("HIDFORGE: input routed to sysapp user %u = %u t=%u\\n", u, routed, GetTickCount());\n'
        '\t\t\t}\n'
        '\t\t}\n'
        '#endif\n'
        '\t\tif (XampInputRoutedToSysapp[c->userIndex]) {\n', 'routed log')

# Analog sticks (shim/sticks.inc): radial inner dead zone against drift, anti
# dead zone so the title's own XInput dead zone does not swallow the first
# quarter of the travel, centred Y inversion. The state of a slot is published
# and read under a sequence counter, so a poll never mixes two reports.
t = sub(t, r'#include "ds3\.inc"\n', '#include "ds3.inc"\n#include "sticks.inc"\n#include "boot.inc"\n', 'sticks include')
# Controllers plugged in at boot (shim/boot.inc): a thread that walks the USB
# ports after the dashboard is up and resets only the nodes no driver claimed,
# so the kernel enumerates them again and the HidAddDevice hook sees them.
t = sub(t, r'(\t\tDbgPrint\("EINTIM: Hooks installed\\n"\);\n)',
        '\t\tDbgPrint("EINTIM: Hooks installed\\n");\n\t\tBootScanStart();\n', 'boot scan start')
t = sub(t, r'\t\tconnectedControllers\[index\]\.currentState = buttonReport;\n',
        '\t\tHfPublishState(index, &connectedControllers[index].currentState, &buttonReport);\n', 'state publish')
t = sub(t, r'b = connectedControllers\[i\]\.currentState;\n',
        'HfReadState(i, &b, &connectedControllers[i].currentState);\n', 'state read')
t = sub(t, r'\t\tinput_state->Gamepad\.sThumbRX = ConvertToFullRange\(b\.z\);\n'
           r'\t\tinput_state->Gamepad\.sThumbRY = ConvertToFullRange\(b\.rz, true\);\n\n'
           r'\t\tinput_state->Gamepad\.sThumbLX = ConvertToFullRange\(b\.x\);\n'
           r'\t\tinput_state->Gamepad\.sThumbLY = ConvertToFullRange\(b\.y, true\);\n',
        '\t\tHfStick(b.z, b.rz, true, &input_state->Gamepad.sThumbRX, &input_state->Gamepad.sThumbRY);\n'
        '\t\tHfStick(b.x, b.y, false, &input_state->Gamepad.sThumbLX, &input_state->Gamepad.sThumbLY);\n',
        'stick conversion')
t = sub(t, r'\t\tinput_state->Gamepad\.bLeftTrigger = b\.rx;\n\t\tinput_state->Gamepad\.bRightTrigger = b\.ry;\n',
        '\t\tinput_state->Gamepad.bLeftTrigger = HfTrigger(b.rx);\n'
        '\t\tinput_state->Gamepad.bRightTrigger = HfTrigger(b.ry);\n', 'trigger shaping')

# Debug aid: the title's own input polling also writes pending log lines.
t = sub(t, r'(DWORD XamInputGetStateHook\(DWORD user, DWORD flags, XINPUT_STATE\* input_state\) \{\n)',
        'DWORD XamInputGetStateHook(DWORD user, DWORD flags, XINPUT_STATE* input_state) {\n\tShimPollLog();\n',
        'poll log')

write('main.cpp', t)

d = read('Detours.h')
d = sub(d, r'#include <xtl.h>\n#include <stdint.h>\n',
        '#include "shim.h"\n\nextern "C" BYTE DetourTrampolineBuffer[200 * 20];\n', 'Detours includes')
d = sub(d, r'[ \t]*static BYTE\s+TrampolineBuffer\[200 \* 20\];\n', '', 'trampoline member')
d = d.replace('Detour::TrampolineBuffer', 'DetourTrampolineBuffer')
d = sub(d, r'Detour::WriteFarBranch\(this->HookSource, this->HookTarget, false, false\);\n\n(\s*)return true;',
        'Detour::WriteFarBranch(this->HookSource, this->HookTarget, false, false);\n'
        '\t\tShimFlushCode(DetourTrampolineBuffer, 200 * 20);\n'
        '\t\tShimFlushCode(this->HookSource, (ULONG)HookSize);\n\n\t\treturn true;',
        'flush after install')
write('Detours.h', d)
write('detours_data.cpp', '#include "Detours.h"\n\nSIZE_T Detour::TrampolineSize = 0;\n')

for name in ('shim.h', 'shim.cpp', 'tramp.S', 'xex.toml', 'rumble.inc', 'ds3.inc', 'sticks.inc', 'boot.inc'):
    shutil.copy(os.path.join(shim, name), os.path.join(out, name))
with open(os.path.join(shim, 'BUCK')) as f:
    buck = f.read()
buck = buck.replace('        "hid_parser.cpp",\n        "mapping.cpp",\n', '')
if 'hid_parser' in buck:
    raise SystemExit('step "BUCK": source list not as expected')
with open(os.path.join(out, 'BUCK'), 'w', newline='\n') as f:
    f.write(buck)
print('ported v0.5 into', out)
