// Analog sticks: 8-bit report bytes to XInput thumb values.
//
// Upstream v0.5 maps each byte linearly, (v - 128) * 256, and inverts Y as
// (~v - 128) * 256, which puts the Y centre at -256. Titles then subtract their
// own XInput dead zone (7849 left, 8689 right, about a quarter of the travel),
// so the first quarter of the stick does nothing and only ~94 of the 127 steps
// per side are left to aim with.
//
// HfStick works on both axes of a stick at once (radial):
//   1. centre: 128 is 0 on both axes; Y is negated after centring;
//   2. inner dead zone: a magnitude up to HF_INNER_DZ counts is 0 (sensor noise,
//      the drift at rest);
//   3. anti dead zone: the rest, HF_INNER_DZ..127 counts, is mapped onto
//      HF_ANTI_DZ..32767, so the first count past the inner dead zone already
//      lands at the title's dead zone edge and every count moves the aim;
//   4. response curve: the travel t (0..1) becomes (1 - k) * t + k * t * t, so
//      with k > 0 the output grows slowly near the centre (fine aim) and still
//      reaches 32767 at the end of the travel;
//   5. the direction is kept: both axes are scaled by the same ratio.
// Integer only (no floating point in this module), 32-bit products only.

#define HF_INNER_DZ_256   (4 * 256)   // inner dead zone, in 1/256 counts: magnitude <= 4 counts is 0.
                                      // Measured on the XE 2: the stick parks up to 2 counts off 128.
// Anti dead zone, ~66 % of the title's XInput dead zone (7849 left, 8689 right). On the console: 8689
// linear was too quick off the centre, 7000 with half square too stiff, 5000/4500 linear good; this
// is a touch looser, inside the 0.10-0.20 range DS4Windows and Steam Input users settle on.
#define HF_ANTI_DZ_L      5200
#define HF_ANTI_DZ_R      5800
#define HF_CURVE_L        0           // curve weight k, 0..256 (256 = full square): linear, like a stock Xbox 360 pad
#define HF_CURVE_R        0
// Outer dead zone: full output at 121 of 127 counts (~95 %), so full-speed turns and diagonals do not
// need the stick pressed into the gate. A stock 360 pad has ~0.5 mm of outer dead zone.
#define HF_RADIUS_256     (121 * 256)

// Triggers: 0..HF_TRIG_DZ is 0 (a resting finger), HF_TRIG_FULL..255 is 255 (the DualSense travel is
// long), linear in between.
#define HF_TRIG_DZ        3
#define HF_TRIG_FULL      245

static uint32_t HfIsqrt(uint32_t v) {
	uint32_t r = 0;
	uint32_t bit = 1u << 30;
	while (bit > v)
		bit >>= 2;
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		}
		else
			r >>= 1;
		bit >>= 2;
	}
	return r;
}

static int16_t HfClamp16(int32_t v) {
	if (v > 32767)
		return 32767;
	if (v < -32767)
		return -32767;
	return (int16_t)v;
}

static void HfStick(uint8_t rawX, uint8_t rawY, bool rightStick, int16_t* outX, int16_t* outY) {
	int32_t x = (int32_t)rawX - 128;   // -128..127, right is positive
	int32_t y = 128 - (int32_t)rawY;   // -127..128, up is positive

	// Magnitude in 1/256 counts. x*x + y*y <= 32768, times 65536 fits in 32 bits unsigned.
	uint32_t m = HfIsqrt((uint32_t)(x * x + y * y) << 16);
	if (m <= HF_INNER_DZ_256) {
		*outX = 0;
		*outY = 0;
		return;
	}

	int32_t anti = rightStick ? HF_ANTI_DZ_R : HF_ANTI_DZ_L;
	int32_t k = rightStick ? HF_CURVE_R : HF_CURVE_L;
	int32_t span = (m > HF_RADIUS_256 ? HF_RADIUS_256 : (int32_t)m) - HF_INNER_DZ_256;
	// Travel in Q15 (0..32767). span <= 31488, times 32767 stays below 2^30.
	int32_t t = span * 32767 / (HF_RADIUS_256 - HF_INNER_DZ_256);
	// t * t + 32767 < 2^31 and rounds t^2 up so 32767 stays 32767; each term <= 2^23.
	int32_t f = ((256 - k) * t + k * ((t * t + 32767) >> 15)) >> 8;
	// f <= 32767 and (32767 - anti) <= 25767: the product stays below 2^30.
	int32_t mag = anti + f * (32767 - anti) / 32767;

	// x * 256 <= 32768 and mag <= 32767: the product stays below 2^30.
	*outX = HfClamp16(x * 256 * mag / (int32_t)m);
	*outY = HfClamp16(y * 256 * mag / (int32_t)m);
}

static uint8_t HfTrigger(uint8_t raw) {
	if (raw <= HF_TRIG_DZ)
		return 0;
	if (raw >= HF_TRIG_FULL)
		return 255;
	// 1..241 onto 1..254, rounded: never 0 once past the dead zone.
	int32_t v = ((int32_t)(raw - HF_TRIG_DZ) * 254 + (HF_TRIG_FULL - HF_TRIG_DZ) / 2) / (HF_TRIG_FULL - HF_TRIG_DZ);
	return (uint8_t)(v < 1 ? 1 : (v > 254 ? 254 : v));
}

#ifndef HF_STICKS_TEST
// PS button state per slot. Starts down, so a PS held while the pad connects
// (the press that switches a DualSense on) only arms the button; see the
// "PS button tap" step in port_v05.py.
static bool g_psWasDown[4] = { true, true, true, true };
#define HF_PS_BOOT_QUIET_MS 20000   // no guide press in the first 20 s after boot

// The interrupt handler writes the state of a slot while a title thread may be
// copying it in XamInputGetStateHook. A sequence counter per slot (odd while a
// write is in progress) lets the reader retry instead of mixing two reports.
static volatile uint32_t g_stateSeq[4];

static void HfPublishState(int slot, ButtonsReport* dst, const ButtonsReport* src) {
	if (slot < 0 || slot > 3)
		return;
	g_stateSeq[slot]++;
	__asm__ volatile("lwsync" ::: "memory");
	*dst = *src;
	__asm__ volatile("lwsync" ::: "memory");
	g_stateSeq[slot]++;
}

static void HfReadState(int slot, ButtonsReport* dst, const ButtonsReport* src) {
	for (int tries = 0; tries < 8; tries++) {
		uint32_t s = g_stateSeq[slot];
		__asm__ volatile("lwsync" ::: "memory");
		*dst = *src;
		__asm__ volatile("lwsync" ::: "memory");
		if (!(s & 1) && s == g_stateSeq[slot])
			return;
	}
}
#endif
