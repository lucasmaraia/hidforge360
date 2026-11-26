# hidforge360

**PlayStation and HID controllers on the Xbox 360, as a Dashlaunch plugin, built with an open toolchain.**

[![CI](https://github.com/lucasmaraia/hidforge360/actions/workflows/ci.yml/badge.svg?branch=hidforge)](https://github.com/lucasmaraia/hidforge360/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/lucasmaraia/hidforge360)](https://github.com/lucasmaraia/hidforge360/releases/latest)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue)](LICENSE)

hidforge360 is a fork of [EinTim23/hiddriver360](https://github.com/EinTim23/hiddriver360) v0.5. It ports that
driver to the open [xenon-forge](https://github.com/landaire/xenon-forge) toolchain and makes it work on consoles
that run Aurora and Dashlaunch from a USB drive. It also adds rumble, the DualShock 3, the Brook Wingman XE 2
adapter and tuned analog sticks. Every release is built, audited and published by GitHub Actions, and the build
is reproducible.

> Documentation outside this README is in Portuguese (pt-BR): see [`docs/`](docs/).

## Improvements over hiddriver360 v0.5

| Area | hiddriver360 v0.5 | hidforge360 |
|---|---|---|
| USB drive boot (Aurora/Dashlaunch on USB) | v0.3+ restarts the whole USB stack on load, which drops the drive the system runs from and hangs the console | **No global USB reset**: works with the system on a USB drive |
| Rumble | none | **DualSense, DualShock 4 and DualShock 3**, over the output endpoint with the control endpoint as fallback. The DS4 lightbar shows the player slot colour |
| DualShock 3 | no | **yes**: enable command, report layout, player LED |
| Wireless pads | no | **Brook Wingman Converter XE 2** (`0c12:0efa`), so a Bluetooth DualSense plays wirelessly with rumble |
| Analog sticks | linear `(v-128)*256`; the title's own XInput dead zone (~25 %) stacks on top; Y centre off by 256 | radial inner dead zone against drift, **anti dead zone** (~66 % of the XInput one), outer dead zone (full output at 95 %), linear response, centred Y, tuned on the console |
| Triggers | raw byte | small dead zone and full press at 96 % of the travel (the DualSense travel is long) |
| Controller plugged in at boot | needs a global USB reset | **resets only the port node no driver claimed**, so the kernel re-enumerates that device and the plugin takes it. The USB drive is never touched |
| PS button | sends the guide on every poll while held (repeats every second) | **one guide press per press**. A press held while the pad connects does not count, and there is no guide in the first 20 s of boot |
| Robustness | pool allocations inside USB callbacks, report data on top of the USB stack's bookkeeping (TRB + 0x1C), reads of `connectedControllers[-1]`, torn state reads | static per-slot buffers, a reserved field guarded by `static_assert`, a slot guard, state behind a sequence counter, a 64 KiB stack, cache flush after the hooks are installed |
| Build | Visual Studio 2010 + Xbox 360 XDK | **open toolchain** (xenon-forge, clang, buck2). Reproducible: the release `.xex` is byte-identical to the console-tested one |
| Safety net | none | every build is **audited**: zero vector/floating point instructions, and every function that keeps conditions in cr2–cr4 across calls is reviewed |
| Diagnostics | debug output | `hidforge-diag.xex` writes `hidforge.log` to the USB drive and shows the driver stage on the ring of light |

## Supported controllers

| Controller | VID:PID | Tested on the console | Notes |
|---|---|---|---|
| DualSense | `054c:0ce6` | plays + rumble | high speed (EHCI) |
| DualShock 4 v2 | `054c:09cc` | plays + rumble | lightbar shows the player slot |
| DualShock 3 | `054c:0268` | plays + rumble | player LED |
| Brook Wingman Converter XE 2 | `0c12:0efa` | plays + rumble | PS4/PC mode, driven as a DualShock 4 |
| DualSense Edge | `054c:0df2` | not tested | same path as the DualSense (upstream detection) |
| DualShock 4 v1, DS4 wireless adapter | `054c:05c4`, `054c:0ba0` | not tested | same path as the DS4 v2 |

Tested on an Xbox 360 model E, kernel 2.0.17559.0, BadUpdate with XeUnshackle, with Aurora and Dashlaunch on a
USB drive. Kernel 17489 is accepted by the code but untested here.

## Install

1. Download `hidforge.xex` from the [latest release](https://github.com/lucasmaraia/hidforge360/releases/latest) and
   check it against `SHA1SUMS`.
2. Copy it to the root of the drive that holds `launch.ini`, and point a plugin slot at it
   (the file is CRLF; keep it that way):
   ```ini
   plugin1 = Usb:\hidforge.xex
   ```
3. Boot the console.

**Usage rules**
- Plug wired controllers in **after Aurora is up**.
- The XE 2 may stay plugged in from boot, but switch the DualSense on **after Aurora is up**. The plugin loads
  about 28 s into the boot and resets the adapter's port, and that drops a pad that is already connected.
- A tap on PS opens the guide. Holding it does nothing.

Full guide (pt-BR): [`docs/instalacao.md`](docs/instalacao.md).

## Build

```sh
docker run --rm -it -v "$PWD:/repo" -w /repo rust:1-bookworm bash tools/build.sh dist
```

[`tools/build.sh`](tools/build.sh) does the following:
- clones xenon-forge at the pinned commit;
- ports the untouched upstream source in [`hiddriver/`](hiddriver/) with
  [`hidforge/port_v05.py`](hidforge/port_v05.py), a series of named text patches that stop the build if upstream
  no longer matches;
- builds the normal and the diagnostic variants;
- runs the stick desk test;
- audits both ELFs with [`tools/audit-elf.sh`](tools/audit-elf.sh).

The output is `dist/hidforge.xex`, `dist/hidforge-diag.xex` and `dist/SHA1SUMS`. To release, push a `vX.Y.Z` tag:
[`release.yml`](.github/workflows/release.yml) builds, audits and publishes it with the matching
[`CHANGELOG.md`](CHANGELOG.md) section.

Development guide (pt-BR): [`docs/desenvolvimento.md`](docs/desenvolvimento.md).

## Known limitations

- A DualSense switched on before Aurora is up, through an XE 2 plugged in at boot, stays connected but dead until
  it is switched on again (see the usage rules).
- A DualSense on a cable plugged in at boot still has to be re-plugged: it is a composite device (audio + HID),
  and the boot scan never resets composite nodes.
- No direct Bluetooth: the Xbox 360 has no Bluetooth stack. Use an adapter that presents itself as a wired pad,
  such as the XE 2.

## Documentation (pt-BR)

| Document | Contents |
|---|---|
| [`docs/instalacao.md`](docs/instalacao.md) | installation, usage, diagnostics, limitations |
| [`docs/desenvolvimento.md`](docs/desenvolvimento.md) | repository layout, build, ELF audit, tests, rules |
| [`docs/conhecimento.md`](docs/conhecimento.md) | everything learned: the 360 USB stack, HID output routes, report formats, boot takeover, the PS button, toolchain pitfalls, mistakes made |
| [`docs/testes/wingman-xe2.md`](docs/testes/wingman-xe2.md) | round-by-round console log of the XE 2 support and the stick calibration |
| [`docs/logs/`](docs/logs/) | raw console and PC logs referenced by the documents |
| [`CHANGELOG.md`](CHANGELOG.md), [`CONTRIBUTING.md`](CONTRIBUTING.md) | changes per version, how to contribute |

## Credits

- **[EinTim23/hiddriver360](https://github.com/EinTim23/hiddriver360)**: the original driver this fork is built on.
  Its v0.5 source is kept intact in [`hiddriver/`](hiddriver/), and its README is preserved as
  [`README.upstream.md`](README.upstream.md).
- **[landaire/xenon-forge](https://github.com/landaire/xenon-forge)**: the open Xbox 360 toolchain and SDK
  headers.
- **[kadirdogan/hiddriver360](https://github.com/kadirdogan/hiddriver360)**: the per-node USB reset technique for
  controllers present at boot, here adapted to kernel 17559.
- **[AustinJMann/tritondriver360](https://github.com/AustinJMann/tritondriver360)**: the reference for working
  rumble and for the TRB + 0x1C layout.
- Report formats from Linux `hid-playstation.c` and `hid-sony.c`, SDL, and USB Host Shield.

## License

[GPL-3.0](LICENSE), the same as hiddriver360.
