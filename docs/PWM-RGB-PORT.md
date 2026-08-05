# Porting a PWM RGB LED fade example to this repo

Status: **implemented and bench-proven.** The example now lives at
`apps/rgb_fade/` — built against Alif's Zephyr fork, run on E1M-AEN801, and
the RGB LED confirmed fading at the board on 2026-08-05. See that app's
README for the run evidence.

This document is kept as the porting record: it explains why the example
builds against Alif's fork rather than the upstream Zephyr the rest of this
repo pins, and the traps found while establishing that.

This file records everything established for adding a PWM RGB LED fade
example to this repo as a bare-Zephyr application, so the next person does
not have to re-derive it. Every hardware fact below is cited to its source.

## 1. Why this is not a small change

Upstream Zephyr **v4.4.0** — the version this repo pins in `west.yml` — has
no PWM support for Alif Ensemble at all. Its entire Alif devicetree surface
is `pinctrl` plus `uart0`..`uart5` (see the root `README.md`). There is no
PWM node, no timer node, and no GPIO node, so there is also no fallback of
bit-banging the LED from GPIO or using a software-PWM driver: both would
need a GPIO driver that upstream does not have for this SoC either.

The PWM support exists in **Alif's own Zephyr fork**,
`alifsemi/zephyr_alif`, and it is what has to be ported.

## 2. The RGB LED wiring

The RGB LED is on the **carrier/EVK**, not on the SoM. Its three channels
are **non-contiguous** in the E1M PWM numbering — do not assume PWM0/1/2:

| Colour | E1M PWM | E1M pad | Silicon peripheral | Silicon pad |
|---|---|---|---|---|
| Green  | PWM0 | `A6` | `UT11_T1_C` | `P12_7` |
| Blue   | PWM1 | `B6` | `UT11_T0_C` | `P12_6` |
| Red    | PWM3 | `B5` | `UT10_T0_A` | `P2_4`  |

So green and blue are two channels of the **same** UTIMER instance
(`utimer11`, driver B / channel 1 and driver A / channel 0 respectively),
and red is on `utimer10`, driver A / channel 0.

The naming convention `UT<n>_T<0|1>_<A|B|C>` is: UTIMER instance `n`,
driver A (channel 0) or driver B (channel 1), and the pad-variant letter.

## 3. What has to be ported

### 3.1 The UTIMER register library (vendored)

From `alifsemi/hal_alif`, Apache-2.0:

- `drivers/utimer/include/utimer.h` — 584 lines, **zero `#include`s**; pure
  register definitions and inline accessors.
- `drivers/utimer/src/utimer.c` — includes only `<stdint.h>`, `<stdbool.h>`
  and `<utimer.h>`.

Both are self-contained and vendor cleanly into an out-of-tree module, the
same way `modules/alif-mhuv2/` already carries a register-level driver.
Preserve the copyright headers.

Note this contradicts a first reading of the Zephyr driver, whose includes
look HAL-free: the dependency is declared in Kconfig, not in the includes —
`drivers/pwm/Kconfig.alif` has `select USE_ALIF_HAL_UTIMER`, and the
`"utimer.h"` include resolves into the HAL module.

### 3.2 The Zephyr PWM driver

From `alifsemi/zephyr_alif`, Apache-2.0:

- `drivers/pwm/pwm_alif_utimer.c` (254 lines), `DT_DRV_COMPAT` is `alif_pwm`
- `drivers/pwm/Kconfig.alif` — `config PWM_ALIF_UTIMER`, `depends on
  DT_HAS_ALIF_PWM_ENABLED`, `select USE_ALIF_HAL_UTIMER`
- `dts/bindings/pwm/alif,pwm.yaml` — `compatible: "alif,pwm"`,
  `#pwm-cells = 3` (channel, period-ns, flags)
- the `alif,utimer` parent binding
- `include/zephyr/dt-bindings/timer/alif_utimer.h`

### 3.3 The clock controller — mostly already upstream, with one trap

Good news first: **upstream v4.4.0 already has the Alif clock controller**,
so most of the feared dependency does not need porting at all. It ships:

- the node `clockctrl: clock-controller@1a602000`, `compatible =
  "alif,clockctrl"`, already instantiated in
  `dts/arm/alif/ensemble/common/ensemble_common.dtsi`
- the driver `drivers/clock_control/clock_control_alif.c` (351 lines,
  `DT_DRV_COMPAT alif_clockctrl`), `CONFIG_CLOCK_CONTROL_ALIF` defaulting on
  when the node is enabled
- the binding `dts/bindings/clock/alif,clockctrl.yaml`
- the headers `dt-bindings/clock/alif-clocks-common.h` and
  `alif-ensemble-clocks.h`

The upstream driver is **generic**: it decodes the `ALIF_CLK_CFG()` encoding
into module ID, register offset and enable bit, and computes
`base + reg_offset`. Nothing about it is UART-specific in the enable path.

**The trap.** The UTIMER's clock ID cannot be expressed to it as-is.

- Upstream's `alif-ensemble-clocks.h` is 62 lines and defines **UART clocks
  only** — there is no `ALIF_UTIMER_CLK`.
- In the Alif fork it is `#define ALIF_UTIMER_CLK ALIF_CLK(1U)`, and
  `ALIF_CLK(value)` expands to `ALIF_CLK_CFG(DUMMY, DUMMY, value, 0U, 0U,
  0U, 0U)` — i.e. a `DUMMY` module with `en_mask = 0` (nothing to write).
  Upstream defines **neither** `ALIF_CLK()` nor a `DUMMY` module, and its
  driver `__ASSERT`s on an unknown module ID.
- Worse, and easy to miss: **the module IDs are renumbered between the two
  trees.** Upstream has `ALIF_CGU_MODULE 0x0`, `CLKCTL_PER_MST 0x1`,
  `CLKCTL_PER_SLV 0x2`, `AON 0x3`, `VBAT 0x4`, `M55HE_CFG 0x5`,
  `M55HP_CFG 0x6`. The fork has CGU `0x1`, PER_MST `0x2`, PER_SLV `0x3`,
  AON `0x4`. **Mixing a fork-built clock ID with the upstream driver
  silently selects the wrong module and writes the wrong register.** Do not
  copy clock IDs across without re-deriving them.

Two ways out, and the second is recommended:

1. Vendor the fork's fuller `clock_control_alif_ensemble.c` (670 lines) plus
   its clock headers. Costly, and it collides with upstream's driver on the
   same `alif,clockctrl` compatible — you would have to disable
   `CONFIG_CLOCK_CONTROL_ALIF` and take over the node.
2. **Drop `clock_control` from the vendored PWM driver.** `ALIF_UTIMER_CLK`
   writes nothing (`en_mask = 0`), so the clock-controller call is not what
   actually gates the timer — the driver separately calls
   `alif_utimer_enable_timer_clock(global_base, cfg->timer_id)`, which is
   the real per-timer enable in the UTIMER global register window. Replace
   the `clock_control_get_rate()` call with a `clock-frequency` devicetree
   property on the utimer node. This is a deliberate divergence from the
   upstream driver and must be commented as such where it is made.

Option 2 has NOT been implemented or tested. It rests on the reading that
the `clockctrl` call is a no-op for this clock ID; confirm that against the
fork's own driver before relying on it.

### 3.4 Devicetree nodes

Upstream v4.4.0 has no UTIMER nodes, so they must be added out-of-tree. The
shape, from the Alif fork's `dts/arm/alif/ensemble/common/e4_e6_e8.dtsi`:

```dts
lputimer0: lputimer@4300d000 {
	compatible = "alif,utimer";
	reg = <0x4300D000 0x1000 0x4300C000 0x24>;
	reg-names = "timer", "global";
	timer-id = <0>;
	clocks = <&clockctrl ALIF_LPUTIMER_CLK>;
	counter-direction = <ALIF_UTIMER_COUNTER_DIRECTION_UP>;
	status = "disabled";

	pwm0 {
		compatible = "alif,pwm";
		#pwm-cells = <3>;
		status = "disabled";
	};
};
```

Note the two-range `reg`: a per-instance timer window plus a **shared global**
window (`0x4300C000`, 0x24 bytes) used for the cross-instance enable.

For the RGB LED the needed instances are **utimer10** and **utimer11**, not
the `lputimer` instances shown above. Both are defined in the fork's
`dts/arm/alif/ensemble/common/e1.dtsi` (not in `e4_e6_e8.dtsi`, which carries
only the `lputimer` instances):

```dts
utimer10: utimer@4800b000 {
	compatible = "alif,utimer";
	reg = <0x4800B000 0x1000 0x48000000 0x24>;
	reg-names = "timer", "global";
	timer-id = <10>;
	clocks = <&clockctrl ALIF_UTIMER_CLK>;   /* see section 3.3 -- do NOT copy this ID */
	counter-direction = <ALIF_UTIMER_COUNTER_DIRECTION_UP>;
	status = "disabled";

	pwm10 { compatible = "alif,pwm"; #pwm-cells = <3>; status = "disabled"; };
};

utimer11: utimer@4800c000 {
	/* identical shape; reg = <0x4800C000 0x1000 0x48000000 0x24>, timer-id = <11> */
};
```

Note both share the same global window at `0x48000000`, and their IRQs are
457-464 (utimer10) and 465-472 (utimer11), named `comp_capt_a`,
`comp_capt_b`, `comp_a_buf1`, `comp_a_buf2`, `comp_b_buf1`, `comp_b_buf2`,
`underflow`, `overflow`. A PWM-only example does not need the interrupts.

So the RGB LED needs **both** instances enabled: green and blue are two
channels of `utimer11`, red is channel 0 of `utimer10`.

## 4. A devicetree trap that will cost a bench cycle

Do **not** use `DEVICE_DT_GET(DT_NODELABEL(pwmN))` on the controller node.
The `alif,pwm` binding re-declares `#pwm-cells`, so `gen_defines` treats the
controller as if it had its own `pwms` phandle and emits a phantom
`pwmN_P_pwms_IDX_0` reference, which fails to build.

Resolve the controller through a **consumer** node instead — a `pwm-leds`
node whose child carries a real `pwms` phandle — and use `PWM_DT_SPEC_GET`,
which dereferences the phandle in the consumer's property via
`DT_PWMS_CTLR_BY_IDX`. This is the pattern the reference example uses, and
the reason it uses it.

That also makes `pwm-leds` a natural fit for an RGB example: three LED
children, one per colour, each with its own `pwms` phandle.

## 5. Pinctrl

Fully covered upstream — nothing to port. v4.4.0 has
`compatible = "alif,pinctrl"` **and** the pinmux macros this example needs
(`PIN_P12_7__UT11_T1_C`, `PIN_P12_6__UT11_T0_C`, `PIN_P2_4__UT10_T0_A`) in
`<zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h>`, confirmed present in
the pinned tree.

These are PWM **output** pads, so no input-enable or bias configuration is
required. (Input-enable/bias handling applies to *sensed* pads — I2C SDA/SCL,
UART RX, SPI MISO — not to outputs.)

## 6. The example itself

A straightforward port of the reference fade: 1 kHz period
(`PERIOD_NS = 1000000`), a 50-step linear duty sweep up and then down with a
20 ms delay per step. For RGB, drive the three channels with phase-shifted
sweeps so the LED cycles through colours rather than fading white.

The reference uses a portable SDK PWM wrapper; a bare-Zephyr port calls
`pwm_set_dt()` / `pwm_set_pulse_dt()` on `PWM_DT_SPEC_GET` handles directly,
with no SDK dependency — matching this repo's existing no-vendor-SDK
property.

## 7. What is NOT established

- **Whether the Secure Enclave must enable the UTIMER clock or power domain,
  or unlock the pads, before any of this responds.** Some peripherals on this
  part need an SE service call first. Not investigated. If the driver's
  `clock_control_on()` succeeds but the timer never counts, look here.
- **The base addresses of `utimer10` and `utimer11`.** Read them from the
  Alif fork's Ensemble dtsi; they are not reproduced here because a wrong
  base address writes to the wrong peripheral.
- **Whether the E1M-AEN carrier in front of you is wired like the EVK.** The
  table in section 2 is the EVK routing. Confirm against your own carrier's
  schematic before trusting it — a wrong pad drives the wrong net.
- **Anything about E1M-AEN401 (E4).** Same caveat as the rest of this repo:
  nothing here has been run on E4 silicon.

## 8. Licensing

Both source repositories are Apache-2.0, as is this repo, so vendoring is
straightforward. Keep the original copyright headers and SPDX identifiers on
every file carried across, and record the upstream commit each file came
from.
