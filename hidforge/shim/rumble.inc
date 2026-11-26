// Rumble for the DualSense, DualShock 4 and DualShock 3 over USB.
//
// Included into the ported main.cpp after `connectedControllers`, so it sees
// the Usbd function pointers the driver resolves. The title thread only
// records the requested motor speeds; transfers are queued from USB
// completion callbacks, so only one output transfer is ever in flight and the
// title thread never touches the USB queue. This is the arrangement of the
// draft rumble support in hiddriver360 PR #129.
//
// Two more things follow AustinJMann/tritondriver360, the fork of this driver
// whose rumble works on an Xbox 360: the transfer block and its payload live
// in kernel pool memory rather than in the plugin image, and an unchanged
// effect is resent periodically. That fork also documents the two routes used
// here: an interrupt OUT endpoint when the controller has one, and a HID
// SET_REPORT on the control endpoint otherwise.
//
// Which route each pad takes, measured on this console (kernel 17559, model E):
//   DualSense  interrupt OUT endpoint, fast bus (EHCI)         works
//   DualShock3 SET_REPORT on the control endpoint              works
//   DualShock4 SET_REPORT on the control endpoint, endpoint    the endpoint
//              as a fallback                                   acknowledges
//                                                              and ignores
//
// On the control endpoint the report ID travels in wValue, `(type << 8) | id`,
// and must NOT be repeated in the data; that is how the DualShock 3 enable
// command and its rumble reports work here. Sending the DualShock 4 report
// with the DualShock 3 wValue and the ID in the data got it refused with
// c0051004.
//
// Report layouts (Linux `hid-playstation.c`, SDL `SDL_hidapi_ps4.c` and the
// USB Host Shield library agree):
//   DualSense  report 0x02, 63 bytes: [1] flags, [3] right, [4] left
//   DualShock4 report 0x05, 32 bytes: [1] flags, [4] right, [5] left,
//              [6..8] lightbar RGB (31 bytes without the ID, shifted by one)
//   DualShock3 report 0x01, 48 bytes of data: [1]/[2] small motor,
//              [3]/[4] big motor, [9] player LED, [10..] LED timing blocks

// Same layout as the transfer block the driver keeps in its HID extension.
struct RumbleTrb {
	DWORD endpoint;
	DWORD callback;
	DWORD savedEndpoint;
	BYTE padding[4];
	BYTE flags;
	BYTE controllerIndex;
	BYTE pad2;
	BYTE endpointIndex;
	void* buffer;
	DWORD length;
};

// One per slot, in pool memory (see shim.h).
struct OutputBlock {
	RumbleTrb trb;
	// The USB stack writes the number of bytes transferred at TRB + 0x1C, which
	// is where the payload used to start: the report ID and the two flag bytes
	// of every report sat in a field the stack owns. tritondriver360 keeps the
	// same four bytes aside, with a static_assert whose message reads "Output
	// report overlaps USB transfer bookkeeping".
	DWORD transferredBytes;
	BYTE report[80];
};

static_assert(__builtin_offsetof(OutputBlock, transferredBytes) == 0x1C,
	"the USB completion byte count is not at TRB + 0x1C any more");
static_assert(__builtin_offsetof(OutputBlock, report) == 0x20,
	"the output report overlaps the USB transfer bookkeeping");
static_assert(sizeof(OutputBlock) <= SHIM_OUTPUT_BLOCK_SIZE,
	"the output block no longer fits the pool allocation");

enum RumblePath {
	RUMBLE_PATH_NONE = 0,
	RUMBLE_PATH_INTERRUPT,
	RUMBLE_PATH_CONTROL,
};

struct RumbleSlot {
	OutputBlock* out;
	deviceHandle* device;
	HidControllerExtension* ext; // control endpoint, and the DS3 enable command
	ControllerType type;
	ULONG reportSize;
	uint16_t controlValue; // wValue of the SET_REPORT: (report type << 8) | ID
	uint16_t controlIndex; // wIndex: the HID interface, already byte-swapped
	uint8_t path;
	uint8_t led;           // DS3 player LED
	uint8_t red;           // DS4 lightbar
	uint8_t green;
	uint8_t blue;
	uint8_t hasEndpoint;   // the interrupt OUT endpoint is open
	uint8_t switched;      // already fell back from the control endpoint
	volatile LONG open;
	volatile LONG busy;
	volatile LONG pending;
	volatile LONG left;
	volatile LONG right;
	volatile LONG lastLeft;
	volatile LONG lastRight;
	volatile DWORD lastSent;
	volatile LONG errors;
	volatile LONG traces;
};

static RumbleSlot g_rumble[4];

static const ULONG DUALSENSE_RUMBLE_REPORT_SIZE = 63;  // report 0x02
static const ULONG DUALSHOCK4_RUMBLE_REPORT_SIZE = 32; // report 0x05, with its ID
static const ULONG DUALSHOCK4_RUMBLE_DATA_SIZE = 31;   // report 0x05, without its ID
static const ULONG DUALSHOCK3_RUMBLE_DATA_SIZE = 48;   // report 0x01, without its ID

// wValue of a HID SET_REPORT: output report type 2, then the report ID.
static const uint16_t DUALSHOCK4_RUMBLE_CONTROL_VALUE = 0x0205;
static const uint16_t DUALSHOCK3_RUMBLE_CONTROL_VALUE = 0x0201;

// Resend an unchanged effect at least this often.
static const DWORD RUMBLE_REFRESH_MS = 250;

// Consecutive refusals on the control endpoint before a DualShock 4 falls back
// to its interrupt OUT endpoint.
static const LONG RUMBLE_CONTROL_FAILURES = 3;

// Every pad here takes its rumble on the interrupt OUT endpoint, including the
// DualShock 3 on the full-speed (OHCI) bus, which was the open question. The
// control endpoint stays as the fallback for a pad that has no output endpoint,
// and it is the one the DualShock 4 refuses outright.

#define RUMBLE_TRACE_LIMIT 8
#define RUMBLE_TRACE(r, ...) do { if ((r)->traces < RUMBLE_TRACE_LIMIT) { (r)->traces++; DbgPrint(__VA_ARGS__); } } while (0)

static void RumblePump(int slot);

static int RumbleSlotFromTransfer(DWORD transferAddress) {
	for (int i = 0; i < 4; i++) {
		if (g_rumble[i].out && (DWORD)&g_rumble[i].out->trb == transferAddress)
			return i;
		if (g_rumble[i].ext && (DWORD)&g_rumble[i].ext->defaultEndpoint == transferAddress)
			return i;
	}
	return -1;
}

int32_t RumbleComplete(DWORD transferAddress, int32_t status) {
	int i = RumbleSlotFromTransfer(transferAddress);
	if (i < 0)
		return 0;
	RumbleSlot* r = &g_rumble[i];
	RUMBLE_TRACE(r, "HIDFORGE: rumble done on slot %d, status %x\n", i, status);
	if (status < 0) {
		r->errors++;
		// The pad refuses the control endpoint: try its output endpoint once.
		if (r->path == RUMBLE_PATH_CONTROL && r->hasEndpoint && !r->switched &&
			r->errors >= RUMBLE_CONTROL_FAILURES) {
			r->switched = 1;
			r->errors = 0;
			r->path = RUMBLE_PATH_INTERRUPT;
			DbgPrint("HIDFORGE: slot %d falls back to the output endpoint\n", i);
		}
		InterlockedExchange(&r->pending, 1);
	}
	else {
		r->errors = 0;
	}
	InterlockedExchange(&r->busy, 0);
	RumblePump(i);
	return 0;
}

// The 48 bytes of DualShock 3 output report 0x01, without its report ID.
static void BuildDualShock3Data(BYTE* b, BYTE left, BYTE right, uint8_t led) {
	b[1] = right ? 0xFE : 0x00; // small motor duration
	b[2] = right ? 0x01 : 0x00; // small motor on
	b[3] = left ? 0xFE : 0x00;  // big motor duration
	b[4] = left;                // big motor force
	b[9] = led;
	for (int i = 0; i < 4; i++) {
		BYTE* t = &b[10 + i * 5];
		t[0] = 0xFF;
		t[1] = 0x27;
		t[2] = 0x10;
		t[3] = 0x00;
		t[4] = 0x32;
	}
}

// Fills the payload and returns its length.
static ULONG BuildReport(RumbleSlot* r, BYTE left, BYTE right) {
	BYTE* b = r->out->report;
	memset(b, 0, sizeof(r->out->report));

	if (r->type == SONY_DUALSENSE) {
		b[0] = 0x02;
		b[1] = 0x01 | 0x02; // compatible vibration, haptics select
		b[3] = right;
		b[4] = left;
		return DUALSENSE_RUMBLE_REPORT_SIZE;
	}

	if (r->type == SONY_DUALSHOCK3) {
		if (r->path == RUMBLE_PATH_INTERRUPT) {
			// On the endpoint the report ID leads the data.
			b[0] = 0x01;
			BuildDualShock3Data(&b[1], left, right, r->led);
			return DUALSHOCK3_RUMBLE_DATA_SIZE + 1;
		}
		// On the control endpoint the ID travels in wValue.
		BuildDualShock3Data(b, left, right, r->led);
		return DUALSHOCK3_RUMBLE_DATA_SIZE;
	}

	// DualShock 4. Flags: 0x01 motors, 0x02 lightbar. Linux sends both bits
	// together even when only one changed, because some third-party pads
	// cancel one when they get the other (hid-playstation.c, dualshock4_output_worker).
	//
	// This pad only takes the report on its interrupt OUT endpoint: on a PC,
	// WriteFile (output endpoint) lights it and buzzes it, while
	// HidD_SetOutputReport (SET_REPORT on the control endpoint) is refused.
	// The control branch below stays for completeness; nothing selects it.
	BYTE red = r->red, green = r->green, blue = r->blue;
	if (r->path == RUMBLE_PATH_CONTROL) {
		// Without the report ID everything moves one byte down.
		b[0] = 0x01 | 0x02;
		b[3] = right;
		b[4] = left;
		b[5] = red;
		b[6] = green;
		b[7] = blue;
		return DUALSHOCK4_RUMBLE_DATA_SIZE;
	}
	b[0] = 0x05;
	b[1] = 0x01 | 0x02;
	b[4] = right;
	b[5] = left;
	b[6] = red;
	b[7] = green;
	b[8] = blue;
	return DUALSHOCK4_RUMBLE_REPORT_SIZE;
}

// Sends the newest requested speeds when no output transfer is in flight.
static void RumblePump(int slot) {
	RumbleSlot* r = &g_rumble[slot];
	if (!r->open || !r->out)
		return;

	DWORD now = GetTickCount();
	if (!r->pending && (DWORD)(now - r->lastSent) < RUMBLE_REFRESH_MS)
		return;
	if (InterlockedCompareExchange(&r->busy, 1, 0) != 0)
		return;
	InterlockedExchange(&r->pending, 0);
	r->lastSent = now;

	BYTE left = (BYTE)r->left;   // heavy, low-frequency motor
	BYTE right = (BYTE)r->right; // light, high-frequency motor
	ULONG length = BuildReport(r, left, right);

	int status;
	if (r->path == RUMBLE_PATH_CONTROL) {
		HidControllerExtension* ext = r->ext;
		ext->byte44 = 0x21;                              // host to device, class, interface
		ext->byte45 = 0x09;                              // SET_REPORT
		ext->word46 = swap_endianness_16(r->controlValue); // report type and ID
		ext->interfaceNumber = r->controlIndex;          // wIndex: the HID interface
		ext->word4A = swap_endianness_16((uint16_t)length);
		ext->dword38 = (DWORD)r->out->report;
		ext->dword3C = length;
		ext->byte34 = 1;
		ext->keyboardCompletionHandler = (DWORD)RumbleComplete;
		ext->defaultEndpoint2 = ext->defaultEndpoint;
		status = UsbdQueueAsyncTransfer(ext->deviceHandle, &ext->defaultEndpoint);
	}
	else {
		// flags, callback and savedEndpoint were set when the endpoint opened.
		r->out->trb.buffer = r->out->report;
		r->out->trb.length = length;
		status = UsbdQueueAsyncTransfer(r->device, &r->out->trb);
	}

	RUMBLE_TRACE(r, "HIDFORGE: rumble queued on slot %d via %s, %d bytes (left %d, right %d): %x\n", slot,
		r->path == RUMBLE_PATH_CONTROL ? "control" : "endpoint", length, left, right, status);
	if (NT_ERROR(status)) {
		InterlockedExchange(&r->pending, 1);
		InterlockedExchange(&r->busy, 0);
	}
}

// Prepares a slot and its pool block. Returns false when the block is missing.
static bool RumbleBegin(int slot, deviceHandle* device, HidControllerExtension* ext, ControllerType type) {
	RumbleSlot* r = &g_rumble[slot];
	if (r->busy) {
		DbgPrint("HIDFORGE: slot %d still has an output transfer pending; rumble off\n", slot);
		r->open = 0;
		return false;
	}
	OutputBlock* out = (OutputBlock*)g_shimOutputBlock[slot];
	memset(r, 0, sizeof(RumbleSlot));
	if (!out) {
		DbgPrint("HIDFORGE: slot %d has no output block; rumble off\n", slot);
		return false;
	}
	memset(out, 0, sizeof(OutputBlock));
	r->out = out;
	r->device = device;
	r->ext = ext;
	r->type = type;
	// The driver has just filled the setup packet with the SET_IDLE request of
	// the kernel HID driver, so wIndex holds the HID interface number, already
	// byte-swapped. Keep a copy: a DualShock 4 carries three audio interfaces
	// before its HID one (interface 3), and a class request sent to interface 0
	// is refused with c0051004.
	r->controlIndex = ext ? ext->interfaceNumber : 0;
	return true;
}

// Opens the interrupt OUT endpoint into the slot's pool block. The transfer
// block must stay at the address Usbd saw when the endpoint was opened.
static bool RumbleOpenEndpoint(int slot, ULONG needed) {
	RumbleSlot* r = &g_rumble[slot];
	usb_endpoint_descriptor* out = UsbdGetEndpointDescriptor(r->device, 0, USB_ENDPOINT_TYPE_INTERRUPT,
		USB_DIRECTION_OUT);
	if (!out) {
		DbgPrint("HIDFORGE: slot %d has no interrupt OUT endpoint\n", slot);
		return false;
	}
	ULONG packet = swap_endianness_16(out->wMaxPacketSize) & 0x7FF;
	if (packet < needed || packet > sizeof(r->out->report)) {
		DbgPrint("HIDFORGE: slot %d OUT packet size %d unusable\n", slot, packet);
		return false;
	}
	NTSTATUS status = UsbdOpenEndpoint(r->device, USB_ENDPOINT_TYPE_INTERRUPT, out->bEndpointAddress, packet,
		out->bInterval, (DWORD*)&r->out->trb);
	if (NT_ERROR(status)) {
		DbgPrint("HIDFORGE: slot %d could not open the OUT endpoint: %x\n", slot, status);
		return false;
	}
	r->out->trb.flags = 1;
	r->out->trb.callback = (DWORD)RumbleComplete;
	r->out->trb.savedEndpoint = r->out->trb.endpoint;
	r->hasEndpoint = 1;
	r->path = RUMBLE_PATH_INTERRUPT;
	DbgPrint("HIDFORGE: slot %d rumble endpoint %02x open (packet %d, interval %d, block %p)\n", slot,
		out->bEndpointAddress, packet, out->bInterval, r->out);
	return true;
}

// The lightbar colour a DualShock 4 gets for its user slot, as on a PS4.
static void RumbleDualShock4Colour(RumbleSlot* r, uint8_t userIndex) {
	static const BYTE colours[4][3] = {
		{ 0x00, 0x00, 0xA0 }, // blue
		{ 0xA0, 0x00, 0x00 }, // red
		{ 0x00, 0xA0, 0x00 }, // green
		{ 0x60, 0x00, 0x60 }, // pink
	};
	const BYTE* c = colours[userIndex & 3];
	r->red = c[0];
	r->green = c[1];
	r->blue = c[2];
}

// DualSense: its interrupt OUT endpoint, which works on this console.
// DualShock 4: the control endpoint leads, the endpoint is the fallback.
static void RumbleAttach(int slot, deviceHandle* device, HidControllerExtension* ext, ControllerType type) {
	if (!RumbleBegin(slot, device, ext, type))
		return;
	RumbleSlot* r = &g_rumble[slot];

	if (type == SONY_DUALSENSE) {
		r->reportSize = DUALSENSE_RUMBLE_REPORT_SIZE;
		if (!RumbleOpenEndpoint(slot, r->reportSize))
			return;
		r->pending = 1; // first report sets the lightbar
		r->open = 1;
		return;
	}

	// DualShock 4: the interrupt OUT endpoint is the only route it answers, so
	// there is no fallback to fall back to.
	r->reportSize = DUALSHOCK4_RUMBLE_REPORT_SIZE;
	r->controlValue = DUALSHOCK4_RUMBLE_CONTROL_VALUE;
	RumbleDualShock4Colour(r, 0);
	if (!RumbleOpenEndpoint(slot, DUALSHOCK4_RUMBLE_REPORT_SIZE))
		return;
	r->pending = 1; // first report sets the lightbar
	r->open = 1;
}

// DualShock 3: the control endpoint, which the enable command uses as well, so
// the slot starts busy and that command's completion releases it. The endpoint
// serves as a fallback. The first report also sets the player LED.
static void RumbleAttachDS3(int slot, deviceHandle* device, HidControllerExtension* ext) {
	if (!RumbleBegin(slot, device, ext, SONY_DUALSHOCK3))
		return;
	RumbleSlot* r = &g_rumble[slot];
	r->reportSize = DUALSHOCK3_RUMBLE_DATA_SIZE;
	r->controlValue = DUALSHOCK3_RUMBLE_CONTROL_VALUE;
	r->led = 0x02; // LED 1 until XAM assigns the user index
	if (!RumbleOpenEndpoint(slot, DUALSHOCK3_RUMBLE_DATA_SIZE + 1))
		r->path = RUMBLE_PATH_CONTROL;
	r->busy = 1; // the enable command owns the control endpoint first
	r->pending = 1;
	r->open = 1;
}

// The player the pad was bound to: LED on a DualShock 3, lightbar on a DS4.
static void RumbleSetPlayer(int slot, uint8_t userIndex) {
	RumbleSlot* r = &g_rumble[slot];
	if (userIndex > 3)
		return;
	if (r->type == SONY_DUALSHOCK3)
		r->led = (uint8_t)(0x02 << userIndex);
	else if (r->type == SONY_DUALSHOCK4)
		RumbleDualShock4Colour(r, userIndex);
	else
		return;
	InterlockedExchange(&r->pending, 1);
}

static void RumbleDetach(int slot) {
	g_rumble[slot].open = 0;
}

// Title thread: record the request only.
static void RumbleRequest(int slot, const XINPUT_VIBRATION* vibration) {
	RumbleSlot* r = &g_rumble[slot];
	if (!r->open || !vibration)
		return;
	LONG left = vibration->wLeftMotorSpeed >> 8;
	LONG right = vibration->wRightMotorSpeed >> 8;
	if (left == r->lastLeft && right == r->lastRight)
		return;
	if (r->lastLeft == 0 && r->lastRight == 0)
		RUMBLE_TRACE(r, "HIDFORGE: title asked slot %d for rumble %d/%d\n", slot, left, right);
	r->lastLeft = left;
	r->lastRight = right;
	InterlockedExchange(&r->left, left);
	InterlockedExchange(&r->right, right);
	InterlockedExchange(&r->pending, 1);
}
