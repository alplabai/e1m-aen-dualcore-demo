# rgb_fade -- RGB LED fade via the Alif UTIMER PWM

Fades the RGB LED through a colour cycle by driving three PWM channels of the
Alif UTIMER. Plain Zephyr: the app calls only `<zephyr/drivers/pwm.h>`. There
is no Alp Lab SDK on any path and no `<alp/*>` header.

## This app builds against ALIF's Zephyr, not upstream

Unlike the dual-core apps in this repo, `rgb_fade` **cannot** be built against
the upstream Zephyr v4.4.0 that this repo's `west.yml` fetches. Upstream has no
Alif PWM driver, no UTIMER devicetree nodes, and no UTIMER clock id. All three
exist in Alif's own fork, so this app uses that instead:

```sh
west init -m https://github.com/alifsemi/sdk-alif alif-ws
cd alif-ws && west update
```

That fetches `alifsemi/zephyr_alif` as `zephyr/` plus `hal_alif` as
`modules/hal/alif`, which together provide:

- `drivers/pwm/pwm_alif_utimer.c` -- the PWM driver (`compatible = "alif,pwm"`)
- `utimer10` / `utimer11` devicetree nodes (`dts/arm/alif/ensemble/common/e1.dtsi`)
- the `alif_utimer_*` register library the driver `select`s via
  `CONFIG_USE_ALIF_HAL_UTIMER`

`docs/PWM-RGB-PORT.md` explains why porting these into the upstream tree
instead is a trap -- the clock module ids are renumbered between the two trees,
so a clock id built against one and used with the other silently selects the
wrong register.

## Build

```sh
export ZEPHYR_BASE=<alif-ws>/zephyr

west build -p always -b alif_e8_dk/ae822fa0e5597xx0/rtss_he -d build/dk <repo>/apps/rgb_fade
```

Verified building against Alif's Zephyr **4.1.0** (`sdk-alif` manifest,
`zephyr_alif` at the pinned revision), producing `zephyr.elf` at
FLASH 36252 B / RAM 4912 B, with `CONFIG_PWM_ALIF_UTIMER=y` and
`CONFIG_USE_ALIF_HAL_UTIMER=y` in the resulting `.config`.

If your Zephyr SDK is newer than the fork's expected version, point the build
at the toolchain directly rather than downgrading the SDK:

```sh
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export CROSS_COMPILE=<zephyr-sdk>/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-
```

## The LED wiring

The RGB LED is on the carrier, and its three channels are **non-contiguous**
in the E1M PWM numbering and split across **two** UTIMER instances:

| Colour | E1M PWM | E1M pad | Silicon peripheral | Silicon pad | UTIMER |
|---|---|---|---|---|---|
| Green | PWM0 | `A6` | `UT11_T1_C` | `P12_7` | utimer11 ch1 |
| Blue  | PWM1 | `B6` | `UT11_T0_C` | `P12_6` | utimer11 ch0 |
| Red   | PWM3 | `B5` | `UT10_T0_A` | `P2_4`  | utimer10 ch0 |

## Two devicetree traps this app works around

**PWM handles come from a `pwm-leds` consumer node, not from the controller
nodes.** `DEVICE_DT_GET` on an `alif,pwm` controller makes the codegen pass
emit a phantom `pwmN_P_pwms_IDX_0` reference and the build fails, because the
binding re-declares `#pwm-cells`. Resolving through a consumer's `pwms`
phandle with `PWM_DT_SPEC_GET` reaches the same devices without it.

**The PWM child nodes carry no devicetree labels.** The SoC dtsi declares them
by name only (`pwm10 { ... }`), so the overlay attaches a label while enabling
each one, which is what lets a `pwms` phandle point at them.

## Status

**Bench-proven on silicon, and the LED was seen working.** Run on E1M-AEN801
(`AE822FA0E5597LS0`, M55-HE) via a J-Link ITCM RAM-run (Flow C) — no MRAM was
written and the resident slot0 image was left untouched. Probe
`DPIDR 0x4C013477`.

The RGB LED itself was **confirmed visually by the maintainer at the board on
2026-08-05**: it fades as intended. That is a human observation rather than an
instrumented one — no scope trace was taken and the pad waveforms were not
measured — but for an LED it is the observation that matters, and it closes
the one gap the register evidence below could not: that the programmed
compare values actually reach the pins.

Console (UART5, 115200), from reset:

```
*** Booting Zephyr OS build f002a4d8499c ***
I: === Alp Lab E1M-AEN RGB fade (Alif UTIMER PWM) ===
I: period 1000000 ns (1000 Hz), 50 steps, 20 ms per step
I: red   -> pwm10 channel 0
I: green -> pwm11 channel 1
I: blue  -> pwm11 channel 0
I: fading -- red/green/blue phase-shifted by a third of a cycle
I: cycle 0
I: cycle 1
```

Two soaks: **83 cycles over 164.9 s** and **54 cycles over 106.6 s**, both
gap-free, cycle interval a steady **~2.013 s** against the 2.000 s the source
asks for. No `not ready`, no `pwm_set_dt(...) failed`, and `CFSR = 0x00000000`
at every halt.

Each colour bound to exactly the controller and channel the pad map predicts.

### Register evidence

There is no scope or camera on that bench, so the LED itself was not observed.
What was measured is that the driver programs the hardware. Sampling both
UTIMER windows three times ~1 s apart while the app ran:

| Address | Register | sample 1 → 2 → 3 |
|---|---|---|
| `0x4800B0A0` | `UTIMER_CNTR` (utimer10) | `000617DE` → `000577F8` → `00006E80` |
| `0x4800B0D0` | `UTIMER_COMPARE_A` (red) | `0002EE00` → `00032C80` → `0002EE00` |
| `0x4800C0A0` | `UTIMER_CNTR` (utimer11) | `0005CD40` → `0005FEDB` → `00011C7F` |
| `0x4800C0D0` | `UTIMER_COMPARE_A` (blue) | `00013880` → `0004E200` → `00013880` |
| `0x4800C0E0` | `UTIMER_COMPARE_B` (green) | `00053FC0` → `0000DAC0` → `00053FC0` |

Steady across all three: `UTIMER_CNTR_PTR = 0x00061A80` (400000 counts) on
both timers, `UTIMER_CNTR_CTRL = 0x00000003`, `UTIMER_COMPARE_CTRL_A =
0x00000909` on both, `UTIMER_COMPARE_CTRL_B = 0x00000909` on utimer11 **only**
(`0x00000000` on utimer10) — matching red using one channel while blue and
green share utimer11. `UTIMER_GLB_CNTR_RUNNING = 0x00000C00` sets bits 10 and
11, i.e. both timers running.

A **baseline dump taken while halted, before `go`, read all-zero across both
windows**, so "changed" is measured against a genuine zero start.

Every observed compare value is an exact multiple of `400000 / 50 = 8000` —
`0x0002EE00` = 192000 (step 24), `0x00032C80` = 208000 (step 26), `0x00013880`
= 80000 (step 10), `0x0004E200` = 320000 (step 40), `0x00053FC0` = 344000
(step 43), `0x0000DAC0` = 56000 (step 7). Each lands exactly on one of the 50
sweep steps, at three different phases; samples 1 and 3 (one full cycle apart)
return to identical values while sample 2 (half a cycle) sits opposite.

### What is still not proven, and observations not chased

- **No scope trace of the pads.** `P2_4`, `P12_6` and `P12_7` were not
  measured with an instrument, so the exact duty and edge timing at the pins
  are uncharacterised. The LED fading confirms the signals reach the pads and
  vary as intended; it does not verify the waveform.
- The console prints two `W: Clock enable not supported` warnings before the
  banner. This is consistent with the UTIMER clock id being a no-op — see
  `../../docs/PWM-RGB-PORT.md` section 3.3, where `ALIF_UTIMER_CLK` expands to
  an `en_mask = 0` entry and the real per-timer enable is the separate HAL
  call — but it was recorded, not root-caused.
- After the timers are configured, 41 words per timer window bus-fault on
  read (`+0x04C…+0x07C`, `+0x098`, `+0x09C`, `+0x0BC`, `+0x0CC`, `+0x0DC`,
  `+0x0EC`), though the same offsets read as zero before. Not diagnosed.
- The UTIMER retains its programming and keeps running across a J-Link
  `loadbin` reset. Not diagnosed.
- Reads were refused while the core was running (`Memory map 'after startup
  completion point' is active`), so each sample is `halt` → read → `go`.
  Halting stops the CPU, not the timer hardware.

The board target is Alif's E8 DevKit — the same silicon as E1M-AEN801, which
is what makes this run valid. Building for this repo's own `e1m_aen` board
against the Alif fork additionally needs those board files to accept the
fork's SoC name (`ae822fa0e5597xx0`, where this repo says `ae822fa0e5597ls0` /
`ae402fa0e5597le0`). The overlay ships under both board filenames; the DevKit
one is what has been built and run.
