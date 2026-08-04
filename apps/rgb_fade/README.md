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

**Built, not yet run on hardware.** The build is verified end to end and the
generated devicetree resolves as intended (`utimer10`/`utimer11` enabled,
pinctrl applied, the three consumer channels bound to the right controller and
channel at a 1 kHz period). It has **not** been executed on silicon, so the
LED has not been observed fading.

The board target above is Alif's own E8 DevKit -- the same silicon as
E1M-AEN801. Building for this repo's own `e1m_aen` board against the Alif fork
additionally needs those board files to accept the fork's SoC name
(`ae822fa0e5597xx0`, where this repo's board files say `ae822fa0e5597ls0` /
`ae402fa0e5597le0`). The overlay is provided under both board filenames; the
DevKit one is the one that has been built.
