// Desk test for shim/sticks.inc, built with the host compiler:
//   gcc -std=c99 -Wall -O2 -I.. -o /tmp/sticks_test sticks_test.c && /tmp/sticks_test
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define HF_STICKS_TEST 1
#include "../shim/sticks.inc"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FALHOU: " __VA_ARGS__); printf("\n"); } } while (0)

static long mag2(int16_t x, int16_t y) { return (long)x * x + (long)y * y; }

int main(void) {
	int16_t x, y;

	HfStick(128, 128, true, &x, &y);
	CHECK(x == 0 && y == 0, "centro deu (%d,%d)", x, y);

	// Noise or a parked offset of up to four counts, any direction, is inside the inner dead zone.
	for (int dx = -4; dx <= 4; dx++)
		for (int dy = -4; dy <= 4; dy++)
			if (dx * dx + dy * dy <= 16) {
				HfStick(128 + dx, 128 + dy, true, &x, &y);
				CHECK(x == 0 && y == 0, "ruido (%d,%d) deu (%d,%d)", dx, dy, x, y);
			}

	// First count out of the inner dead zone lands at the title's dead zone edge.
	HfStick(133, 128, true, &x, &y);
	CHECK(x >= HF_ANTI_DZ_R && x < HF_ANTI_DZ_R + 600 && y == 0, "primeiro degrau direito deu %d", x);
	HfStick(133, 128, false, &x, &y);
	CHECK(x >= HF_ANTI_DZ_L && x < HF_ANTI_DZ_L + 600 && y == 0, "primeiro degrau esquerdo deu %d", x);

	// Full deflection.
	HfStick(255, 128, true, &x, &y); CHECK(x == 32767 && y == 0, "direita deu (%d,%d)", x, y);
	HfStick(1, 128, true, &x, &y);   CHECK(x == -32767 && y == 0, "esquerda deu (%d,%d)", x, y);
	HfStick(0, 128, true, &x, &y);   CHECK(x == -32767 && y == 0, "esquerda 0 deu (%d,%d)", x, y);
	HfStick(128, 1, true, &x, &y);   CHECK(x == 0 && y == 32767, "cima deu (%d,%d)", x, y);
	HfStick(128, 255, true, &x, &y); CHECK(x == 0 && y == -32767, "baixo deu (%d,%d)", x, y);

	// Y inversion is symmetric around the centre.
	for (int d = 5; d <= 127; d++) {
		int16_t ux, uy, dx2, dy2;
		HfStick(128, 128 - d, true, &ux, &uy);
		HfStick(128, 128 + d, true, &dx2, &dy2);
		CHECK(uy == -dy2 && ux == 0 && dx2 == 0, "Y assimetrico em %d: %d / %d", d, uy, dy2);
	}

	// Monotonic along an axis: every count moves the output, never backwards.
	int16_t prev = 0;
	for (int v = 133; v <= 128 + 121; v++) {
		HfStick(v, 128, true, &x, &y);
		CHECK(x > prev, "nao cresce em %d: %d <= %d", v, x, prev);
		prev = x;
	}

	// Every input stays inside the circle and keeps its direction (sign).
	for (int a = 0; a < 256; a++)
		for (int b = 0; b < 256; b++) {
			HfStick(a, b, true, &x, &y);
			CHECK(mag2(x, y) <= 32768L * 32768L, "fora do circulo em (%d,%d): (%d,%d)", a, b, x, y);
			CHECK(!((a > 128 && x < 0) || (a < 128 && x > 0)), "sinal X trocado em (%d,%d)", a, b);
			CHECK(!((b < 128 && y < 0) || (b > 128 && y > 0)), "sinal Y trocado em (%d,%d)", a, b);
			if (failures > 20) { printf("muitas falhas\n"); return 1; }
		}

	// Left stick is monotonic too, and both reach full at the end with the curve.
	prev = 0;
	for (int v = 133; v <= 128 + 121; v++) {
		HfStick(v, 128, false, &x, &y);
		CHECK(x > prev, "esquerdo nao cresce em %d: %d <= %d", v, x, prev);
		prev = x;
	}
	HfStick(128, 1, false, &x, &y); CHECK(x == 0 && y == 32767, "esquerdo cima deu (%d,%d)", x, y);

	// The right stick's first step is below the round 1 value (8689 + one linear step).
	{
		int16_t r1, l1, r2, l2, yy;
		HfStick(133, 128, true, &r1, &yy);
		CHECK(r1 >= HF_ANTI_DZ_R && r1 < 8689, "primeiro degrau direito %d nao ficou abaixo de 8689", r1);
		// Near the centre the curved right stick climbs slower than the linear left one.
		HfStick(133, 128, false, &l1, &yy);
		HfStick(150, 128, true, &r2, &yy);
		HfStick(150, 128, false, &l2, &yy);
#if HF_CURVE_R > HF_CURVE_L
		CHECK((r2 - r1) < (l2 - l1), "curva: direito subiu %d, esquerdo %d", r2 - r1, l2 - l1);
#endif
		// Linear right stick: the first step is the anti dead zone plus one step, the end is full.
		CHECK(HF_CURVE_R > 0 || (r1 - HF_ANTI_DZ_R) < 300, "direito linear: primeiro degrau %d longe de %d", r1, HF_ANTI_DZ_R);
		printf("de 5 a 22 contagens: direito %d -> %d, esquerdo %d -> %d\n", r1, r2, l1, l2);
	}

	// Outer dead zone: 121 counts is already full, on every axis.
	HfStick(128 + 121, 128, true, &x, &y);  CHECK(x == 32767, "121 contagens a direita deu %d", x);
	HfStick(128 - 121, 128, false, &x, &y); CHECK(x == -32767, "121 contagens a esquerda deu %d", x);
	HfStick(128, 128 - 121, true, &x, &y);  CHECK(y == 32767, "121 contagens para cima deu %d", y);
	HfStick(128 + 120, 128, true, &x, &y);  CHECK(x < 32767, "120 contagens ja deu 32767");

	// Triggers.
	for (int v = 0; v <= HF_TRIG_DZ; v++) CHECK(HfTrigger(v) == 0, "gatilho %d deu %d", v, HfTrigger(v));
	for (int v = HF_TRIG_FULL; v <= 255; v++) CHECK(HfTrigger(v) == 255, "gatilho %d deu %d", v, HfTrigger(v));
	for (int v = HF_TRIG_DZ + 1; v < HF_TRIG_FULL; v++)
		CHECK(HfTrigger(v) > HfTrigger(v - 1) && HfTrigger(v) < 255, "gatilho nao cresce em %d: %d", v, HfTrigger(v));
	printf("gatilho: 4 -> %d, 128 -> %d, 244 -> %d\n", HfTrigger(4), HfTrigger(128), HfTrigger(244));

	// Diagonal at the gate keeps magnitude near full.
	HfStick(218, 38, true, &x, &y);
	printf("diagonal (218,38) -> (%d,%d)\n", x, y);
	HfStick(140, 128, true, &x, &y);
	printf("12 contagens -> %d (antes: %d)\n", x, (140 - 128) * 256);

	printf(failures ? "%d falha(s)\n" : "todos os testes passaram\n", failures);
	return failures ? 1 : 0;
}
