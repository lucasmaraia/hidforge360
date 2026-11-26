"""Turns the hiddriver360 sources into sources that build with xenon-forge.

usage: port.py <upstream hiddriver dir> <shim dir> <output dir>

The upstream files are not edited by hand: every change is a named step
here, and a step that no longer matches the upstream text stops the script.
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


# --------------------------------------------------------------------------
# main.cpp
# --------------------------------------------------------------------------
t = read('main.cpp')

t = cut(t, '#include <xtl.h>', 'Detour HidAddDeviceDetour;',
        '#include "shim.h"\n#include "Detours.h"\n#include "hid_parser.h"\n#include "usb.h"\n#include "mapping.h"\n\n',
        'includes')

# Thread helper and notifications are only used by the interactive mapping.
t = cut(t, '// This console likes to kill non system threads', 'struct UsbTrb {', '', 'MakeThread/XNotifyUI')
t = sub(t, r'#define XNOTIFYUI_CUSTOM [^\n]*\n', '', 'XNOTIFYUI_CUSTOM')

t = sub(t, r'struct __declspec\(align\(2\)\) HidControllerExtension',
        'struct __attribute__((aligned(2))) HidControllerExtension', 'align(2)')
t = sub(t, r'\} __declspec\(align\(4\)\);', '} __attribute__((aligned(4)));', 'align(4)', count=2)
t = t.replace('unsigned __int8', 'uint8_t')

# Interactive mapping of unknown controllers (needs the C++ standard library).
t = cut(t, 'unsigned int __stdcall MappingThreadProc(void* param);',
        'int interruptHandler(DWORD deviceHandle, int32_t a2) {', '', 'mapping threads')

t = sub(t, r'XInputGetState\(user, &state\);', 'XamInputGetState(user, XINPUT_FLAG_GAMEPAD, &state);',
        'XInputGetState')
t = t.replace('XboxKrnlVersion->', 'XboxKrnlVersion.')
t = sub(t, r'HalSendSMCMessage\(Input, Output\);',
        'HalSendSMCMessage((PSMC_MESSAGE)Input, (PSMC_MESSAGE)Output);', 'SMC message types')

# Names the driver uses for its own function pointers but that the SDK
# headers declare as imports.
for name in ('MmFreePhysicalMemory', 'XamInputSetState'):
    t, n = re.subn(r'\b%s\b' % name, 'g_' + name, t)
    if not n:
        raise SystemExit('rename %s: no match' % name)

# 17559 retail: never reset the USB stack. The reset drops the USB drive the
# dashboard runs from on ABadAvatar setups, which hangs the console. Without
# the reset, the kernel patches that make a second UsbdDriverEntry safe are
# not needed either.
t = sub(t,
        r'UsbdPowerDownNotification = \(usbd_powerdown_notification_func_t\)0x800D8FC8;.*?UsbPhysicalPage = 0x801A8098;',
        'g_SkipUsbDriverReset = true; // hidforge: keep the USB stack running on 17559', 'no USB reset')

# XAM notification patches only serve the mapping notifications.
t = cut(t, '\tif (XNotifyTypeLimitPtr)\n', '\tif (!UsbdGetInterfaceDescriptor || !XamUserBindDeviceCallback ||',
        '', 'XNotify patches')

t = sub(t, r'BOOL APIENTRY DllMain\(HANDLE Handle, DWORD Reason, PVOID Reserved\) \{\n\tif \(Reason == DLL_PROCESS_ATTACH\) \{\n',
        'extern "C" BOOL DllMain(HANDLE Handle, DWORD Reason, PVOID Reserved) {\n'
        '\tif (Reason == DLL_PROCESS_ATTACH) {\n'
        '\t\tif (!ShimInit())\n\t\t\treturn FALSE;\n'
        '\t\tDbgPrint("HIDFORGE: build " __DATE__ " " __TIME__ ", module %p\\n", Handle);\n',
        'DllMain entry')
t = cut(t, 'DbgPrint("EINTIM: Loading mappings!', 'HidAddDeviceDetour = Detour(', '', 'load mappings')
t = sub(t, r'[ \t]*// Start mapping manager thread\n[^\n]*MappingManagerThreadProc[^\n]*\n', '', 'mapping manager')
t = sub(t, r'17502 mode keeps the USB stack running', 'Keeping the USB stack running', 'log text')

write('main.cpp', t)

# --------------------------------------------------------------------------
# Detours.h: the trampoline buffer moves to an assembly file, because clang
# does not place a data object in a code section from C++.
# --------------------------------------------------------------------------
d = read('Detours.h')
d = sub(d, r'#include <xtl.h>\n#include <stdint.h>\n',
        '#include "shim.h"\n\nextern "C" BYTE DetourTrampolineBuffer[200 * 20];\n', 'Detours includes')
d = sub(d, r'[ \t]*#pragma section\(".text", read, execute\)\n[^\n]*TrampolineBuffer\[200 \* 20\];\n', '',
        'trampoline member')
d = d.replace('Detour::TrampolineBuffer', 'DetourTrampolineBuffer')
if 'TrampolineBuffer[' not in d.replace('DetourTrampolineBuffer[', ''):
    pass
write('Detours.h', d)
write('detours_data.cpp', '#include "Detours.h"\n\nSIZE_T Detour::TrampolineSize = 0;\n')

# --------------------------------------------------------------------------
# mapping.cpp: keep the static tables, drop the JSON storage.
# --------------------------------------------------------------------------
m = read('mapping.cpp')
m = cut(m, '#include "mapping.h"', 'static const HidAxisMapEntry kDefaultAxisMap[]', '#include "mapping.h"\n\n',
        'mapping includes')
a = m.find('// A deque never moves existing elements')
if a < 0:
    raise SystemExit('step "mapping tail": marker not found')
m = m[:a] + '''HidDeviceMapping* FindStaticMapping(uint16_t vid, uint16_t pid) {
    for (unsigned i = 0; i < sizeof(kStaticDeviceMappings) / sizeof(kStaticDeviceMappings[0]); i++) {
        HidDeviceMapping& m = kStaticDeviceMappings[i];
        if (m.vendorId == vid && m.productId == pid)
            return &m;
    }
    return nullptr;
}

HidDeviceMapping* FindMapping(uint16_t vid, uint16_t pid) {
    return FindStaticMapping(vid, pid);
}
'''
write('mapping.cpp', m)

# --------------------------------------------------------------------------
# Unchanged upstream files and the shim.
# --------------------------------------------------------------------------
for name in ('hid_parser.cpp', 'hid_parser.h', 'usb.h'):
    write(name, read(name))
for name in ('shim.h', 'shim.cpp', 'mapping.h', 'tramp.S', 'xex.toml', 'BUCK'):
    shutil.copy(os.path.join(shim, name), os.path.join(out, name))

# Headers of the hosted C library that the upstream files include.
write('compat/string.h', '#pragma once\n#include "shim.h"\n')
write('compat/malloc.h', '#pragma once\n#include "shim.h"\n')
print('ported into', out)
