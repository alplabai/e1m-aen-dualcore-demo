/*
 * SPDX-FileCopyrightText: Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rgb_fade -- fade the E1M-AEN carrier's RGB LED through a colour cycle
 * using the Alif UTIMER in PWM mode.
 *
 * This is a plain Zephyr application: it calls the standard
 * <zephyr/drivers/pwm.h> API and nothing else. There is no Alp Lab SDK on
 * any path, no <alp/*> header, and no portable-API wrapper -- the three
 * channels are ordinary `struct pwm_dt_spec` handles resolved from
 * devicetree.
 *
 * WHY THE HANDLES COME FROM A pwm-leds CONSUMER NODE, not from the pwm
 * controller nodes directly:
 *
 *   DEVICE_DT_GET(DT_NODELABEL(pwm11)) on an "alif,pwm" controller node
 *   makes gen_defines emit a phantom `pwm11_P_pwms_IDX_0` reference and the
 *   build fails. The cause is that the "alif,pwm" binding re-declares
 *   #pwm-cells, so the controller looks to the codegen pass as though it
 *   carried its own `pwms` phandle.
 *
 *   Resolving through a consumer instead avoids it: the `rgb_leds` node in
 *   the board overlay is a `pwm-leds` node whose children each hold a real
 *   `pwms` phandle, and PWM_DT_SPEC_GET dereferences THAT property (via
 *   DT_PWMS_CTLR_BY_IDX -> DEVICE_DT_GET(DT_PWMS_CTLR...)) to reach the
 *   controller device. Same devices, no phantom.
 *
 * The LED channels are non-contiguous in the E1M PWM numbering and are split
 * across two UTIMER instances -- see the board overlay for the pad map:
 *
 *   green = E1M PWM0 = UT11_T1_C (utimer11 channel 1) on silicon pad P12_7
 *   blue  = E1M PWM1 = UT11_T0_C (utimer11 channel 0) on silicon pad P12_6
 *   red   = E1M PWM3 = UT10_T0_A (utimer10 channel 0) on silicon pad P2_4
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(rgb_fade, LOG_LEVEL_INF);

/* 1 kHz carrier: fast enough that the eye integrates it, slow enough that
 * the UTIMER's compare resolution still gives a smooth ramp. */
#define PERIOD_NS  1000000U

/* Steps per half-cycle, and how long each step is held. One full triangle
 * (up then down) per channel therefore takes 2 * STEPS * STEP_MS. */
#define STEPS      50U
#define STEP_MS    20U

/* Each channel is phase-shifted by a third of the cycle so the LED walks
 * through the colour wheel instead of fading white in and out together. */
#define PHASE_STEP ((2U * STEPS) / 3U)

struct rgb_channel {
	struct pwm_dt_spec spec;
	const char        *name;
	uint32_t           phase;
};

static struct rgb_channel channels[] = {
	{
		.spec  = PWM_DT_SPEC_GET(DT_NODELABEL(rgb_led_red)),
		.name  = "red",
		.phase = 0U,
	},
	{
		.spec  = PWM_DT_SPEC_GET(DT_NODELABEL(rgb_led_green)),
		.name  = "green",
		.phase = PHASE_STEP,
	},
	{
		.spec  = PWM_DT_SPEC_GET(DT_NODELABEL(rgb_led_blue)),
		.name  = "blue",
		.phase = 2U * PHASE_STEP,
	},
};

/*
 * Triangle wave over a 2*STEPS period: ramps 0 -> STEPS -> 0. Returns the
 * duty as a step count, which the caller scales to nanoseconds.
 */
static uint32_t triangle(uint32_t pos)
{
	pos %= (2U * STEPS);
	return (pos < STEPS) ? pos : ((2U * STEPS) - pos);
}

int main(void)
{
	LOG_INF("=== Alp Lab E1M-AEN RGB fade (Alif UTIMER PWM) ===");
	LOG_INF("period %u ns (%u Hz), %u steps, %u ms per step", PERIOD_NS,
		1000000000U / PERIOD_NS, STEPS, STEP_MS);

	for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
		const struct rgb_channel *ch = &channels[i];

		if (!pwm_is_ready_dt(&ch->spec)) {
			LOG_ERR("%s: PWM device %s not ready -- is its utimer node "
				"enabled in the board overlay?",
				ch->name, ch->spec.dev ? ch->spec.dev->name : "(null)");
			return -ENODEV;
		}

		LOG_INF("%-5s -> %s channel %u", ch->name, ch->spec.dev->name,
			ch->spec.channel);
	}

	/* Start every channel at 0 % so the LED is dark until the sweep runs,
	 * and so a failure below leaves it off rather than at full brightness. */
	for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
		int ret = pwm_set_dt(&channels[i].spec, PERIOD_NS, 0U);

		if (ret != 0) {
			LOG_ERR("%s: pwm_set_dt(0%%) failed: %d", channels[i].name, ret);
			return ret;
		}
	}

	LOG_INF("fading -- red/green/blue phase-shifted by a third of a cycle");

	for (uint32_t tick = 0U;; tick++) {
		for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
			struct rgb_channel *ch    = &channels[i];
			uint32_t            step  = triangle(tick + ch->phase);
			uint32_t            pulse = (PERIOD_NS / STEPS) * step;
			int                 ret   = pwm_set_dt(&ch->spec, PERIOD_NS, pulse);

			if (ret != 0) {
				LOG_ERR("%s: pwm_set_dt(%u ns) failed: %d", ch->name,
					pulse, ret);
				return ret;
			}
		}

		/* One line per full cycle -- enough to see it is alive on the
		 * console without flooding it at 50 Hz. */
		if ((tick % (2U * STEPS)) == 0U) {
			LOG_INF("cycle %u", tick / (2U * STEPS));
		}

		k_msleep(STEP_MS);
	}

	return 0;
}
