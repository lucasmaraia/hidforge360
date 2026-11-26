// DualShock 3 (054c:0268) over USB.
//
// The DS3 sends no input until it gets the enable command: a HID SET_REPORT
// of feature report 0xF4 with 42 0C 00 00, the command hiddriver360 PR #129
// and the USB Host Shield PS3USB driver send. Its input report 0x01 has a
// fixed layout:
//   [2] select, L3, R3, start, up, right, down, left (bit 0 to 7)
//   [3] L2, R2, L1, R1, triangle, circle, cross, square
//   [4] bit 0: PS
//   [6] LX  [7] LY  [8] RX  [9] RY  [18] L2 pressure  [19] R2 pressure

static const uint16_t DUALSHOCK3_PRODUCT_ID = 0x0268;

static BYTE g_ds3Enable[4][8] __attribute__((aligned(16)));

int32_t DS3EnableComplete(DWORD transferAddress, int32_t status) {
	if (status < 0)
		DbgPrint("HIDFORGE: DS3 enable command failed: %x\n", status);
	else
		DbgPrint("HIDFORGE: DS3 enable command accepted\n");
	// The control transfer block is free again: rumble and the LED can use it.
	int slot = RumbleSlotFromTransfer(transferAddress);
	if (slot >= 0) {
		InterlockedExchange(&g_rumble[slot].busy, 0);
		RumblePump(slot);
	}
	return 0;
}

// Sends the enable command on the default endpoint, through the control
// transfer block of the HID extension (the same fields the kernel HID driver
// uses: transfer block at +0x24, setup packet at +0x44).
static void DS3Enable(int slot, HidControllerExtension* ext) {
	BYTE* data = g_ds3Enable[slot];
	data[0] = 0x42;
	data[1] = 0x0C;
	data[2] = 0x00;
	data[3] = 0x00;

	ext->byte44 = 0x21;                          // host to device, class, interface
	ext->byte45 = 0x09;                          // SET_REPORT
	ext->word46 = swap_endianness_16(0x03F4);    // feature report 0xF4
	ext->interfaceNumber = g_rumble[slot].controlIndex; // wIndex: the HID interface
	ext->word4A = swap_endianness_16(4);         // wLength
	ext->dword38 = (DWORD)data;
	ext->dword3C = 4;
	ext->byte34 = 1;
	ext->keyboardCompletionHandler = (DWORD)DS3EnableComplete;
	ext->defaultEndpoint2 = ext->defaultEndpoint;

	int status = UsbdQueueAsyncTransfer(ext->deviceHandle, &ext->defaultEndpoint);
	DbgPrint("HIDFORGE: DS3 enable command queued: %x\n", status);
}

static void DS3Fill(const uint8_t* r, ButtonsReport* b) {
	uint8_t b1 = r[2];
	uint8_t b2 = r[3];

	b->create = (b1 >> 0) & 1; // select
	b->l3 = (b1 >> 1) & 1;
	b->r3 = (b1 >> 2) & 1;
	b->options = (b1 >> 3) & 1; // start

	bool up = (b1 >> 4) & 1;
	bool right = (b1 >> 5) & 1;
	bool down = (b1 >> 6) & 1;
	bool left = (b1 >> 7) & 1;
	uint8_t hat = HAT_NEUTRAL;
	if (up && right) hat = HAT_UP_RIGHT;
	else if (right && down) hat = HAT_DOWN_RIGHT;
	else if (down && left) hat = HAT_DOWN_LEFT;
	else if (left && up) hat = HAT_UP_LEFT;
	else if (up) hat = HAT_UP;
	else if (right) hat = HAT_RIGHT;
	else if (down) hat = HAT_DOWN;
	else if (left) hat = HAT_LEFT;
	b->hatSwitch = hat;

	b->l2 = (b2 >> 0) & 1;
	b->r2 = (b2 >> 1) & 1;
	b->l1 = (b2 >> 2) & 1;
	b->r1 = (b2 >> 3) & 1;
	b->triangle = (b2 >> 4) & 1;
	b->circle = (b2 >> 5) & 1;
	b->cross = (b2 >> 6) & 1;
	b->square = (b2 >> 7) & 1;

	b->ps = r[4] & 1;

	b->x = r[6];
	b->y = r[7];
	b->z = r[8];
	b->rz = r[9];
	b->rx = r[18]; // left trigger pressure
	b->ry = r[19]; // right trigger pressure
}
