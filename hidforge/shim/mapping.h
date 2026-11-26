// Static controller mappings only. The upstream driver also learns unknown
// controllers interactively and stores them as JSON; that part needs the C++
// standard library and is left out of this build.
#pragma once
#include <stdint.h>
#include "usb.h"

struct HidAxisMapEntry {
    uint16_t usage;
    int16_t ButtonsReport::*field;
};

struct HidButtonMapEntry {
    uint8_t idx;
    uint8_t ButtonsReport::*field;
};

struct HidAxisInvertFlags {
    bool invertX;
    bool invertY;
    bool invertZ;
    bool invertRX;
    bool invertRY;
    bool invertRZ;
};

struct HidDeviceMapping {
    uint16_t vendorId;
    uint16_t productId;

    const HidAxisMapEntry *axisMap;
    uint8_t axisMapCount;

    const HidButtonMapEntry *buttonMap;
    uint8_t buttonMapCount;

    HidAxisInvertFlags invert;
};

HidDeviceMapping *FindStaticMapping(uint16_t vid, uint16_t pid);
HidDeviceMapping *FindMapping(uint16_t vid, uint16_t pid);
