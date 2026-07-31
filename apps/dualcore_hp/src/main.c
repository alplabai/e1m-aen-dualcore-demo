/*
 * Copyright 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * dualcore_hp -- IPC-service RPMsg HOST demo, rtss_hp cluster of the Alp Lab
 * E1M-AEN SoM (Alif Ensemble E4/E8, ae402fa0e5597le0 / ae822fa0e5597ls0).
 *
 * WHAT THIS DEMONSTRATES: a minimal, from-scratch dual-core RPMsg link built
 * entirely on upstream Zephyr (ipc_service + the OpenAMP/RPMsg static-vrings
 * backend) plus one small out-of-tree mailbox driver (modules/alif-mhuv2,
 * CONFIG_MBOX_ALIF_MHUV2) for the Alif MHUv2 doorbell hardware that backend
 * needs to signal the other core. Nothing here depends on alp-sdk.
 *
 * ROLE: this cluster is the RPMsg "host" -- see the ipc0 node's
 * role = "host" property in boards/e1m_aen_*_rtss_hp.overlay. Its counterpart
 * is apps/dualcore_he, the "remote", running on the SAME silicon's rtss_he
 * cluster out of the other half of the split MRAM (see the board .dts
 * comments for the partition layout). Both images must be flashed together;
 * neither one boots the whole board's peripherals -- each M55 cluster is an
 * independent Zephyr instance that only knows about its own core.
 *
 * PROTOCOL: every PING_PERIOD_MS this thread sends a struct ping_pong_msg
 * carrying a monotonically increasing sequence number, then blocks for the
 * matching PONG. The remote echoes every PING back verbatim (see
 * apps/dualcore_he/src/main.c). Round-trip time is measured with the core's
 * own cycle counter (k_cycle_get_32/k_cyc_to_us_floor32) captured at send
 * and at receive -- both timestamps are taken on THIS core, so there is no
 * cross-core clock to synchronize.
 *
 * HOW TO RUN: flash this image to e1m_aen/<soc>/rtss_hp and dualcore_he's
 * image to e1m_aen/<soc>/rtss_he (same <soc>), then watch two serial
 * terminals: this core's console is uart5 (E1M UART0), the remote's is uart3 (E1M UART1) (see the
 * "console assignment" comment in the board .dts -- the E1M carrier pinout
 * for these UARTs is NOT yet confirmed against real hardware).
 *
 * SECURE-ENCLAVE BOOT: this core is the one with SE access, so it is also
 * the one that asks the Secure Enclave to release its peer core before the
 * RPMsg link has any chance of working -- see the alif_se_boot_cpu() call
 * near the top of main(), modules/alif-se-boot for the client, and
 * docs/BENCH-DUALCORE.md for the exact sequence PROVEN on E1M-AEN801
 * silicon on 2026-07-30 (369 consecutive PONGs, no gaps). The peer's
 * SE-domain CPU id and reported entry address are Kconfig options
 * (CONFIG_DEMO_RELEASE_PEER_CPU_ID / CONFIG_DEMO_RELEASE_PEER_ENTRY, see
 * ../Kconfig), not literals in this file -- their defaults already encode
 * the bench-proven shape (release M55-HP, entry 0x50000000), which is the
 * MIRROR of what this file used to hardcode (release M55-HE at
 * 0x58000000).
 *
 * alif_se_boot_cpu() alone (SE service_id 501, BOOT_CPU) is the call
 * PROVEN on hardware for this release. alif_se_start_cpu() also exists
 * (modules/alif-se-boot) and additionally runs SET_VTOR (service 505)
 * first -- that ordering is a HYPOTHESIS derived from the SE service enum
 * and a bench failure, and has NEVER been exercised on hardware; this file
 * deliberately calls alif_se_boot_cpu() directly instead. On the
 * bench-proven path, the released core comes up with VTOR == 0x00000000
 * and fetches its initial SP/PC from its own LOCAL address 0 regardless of
 * what entry address was reported here -- see CONFIG_DEMO_RELEASE_PEER_ENTRY's
 * help text and docs/BENCH-DUALCORE.md for why the debugger, not this SE
 * call, is what actually places and starts the peer's image on that path.
 *
 * Ordering is deliberate: ipc_service_open_instance() on the HOST side
 * (this core) does not block waiting for the remote -- only the REMOTE
 * blocks in its own open call, waiting for this core's virtio DRIVER_OK.
 * So releasing the peer first, then opening as host, is safe either way:
 * if the release fails, this core still proceeds to open ipc0 and shows
 * this side of the demo running, with a clear log line explaining why the
 * peer never binds an endpoint.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/time_units.h>

#include <alif_se_boot.h>

LOG_MODULE_REGISTER(dualcore_hp, LOG_LEVEL_INF);

/* Console node name (e.g. "uart@4901c000"), read straight off the
 * `zephyr,console` chosen node this board target picked -- NEVER a literal
 * UART/core name in this file. A previous version of this banner hardcoded
 * "uart5 (E1M UART0)", which is wrong the moment this image is built for a
 * different board target.
 */
#define CONSOLE_NODE_NAME DT_NODE_FULL_NAME(DT_CHOSEN(zephyr_console))

/* The peer core's SE-domain CPU id and reported entry address are Kconfig
 * options now (../Kconfig: CONFIG_DEMO_RELEASE_PEER_CPU_ID /
 * CONFIG_DEMO_RELEASE_PEER_ENTRY), not literals here -- see this file's
 * top-of-file "SECURE-ENCLAVE BOOT" comment and ../Kconfig's help text for
 * why, and for what each default encodes.
 */

/* Cadence of the PING loop. Not a hardware constant -- a demo-legibility
 * choice so a watcher reading two serial terminals can follow the exchange.
 */
#define PING_PERIOD_MS 500

/* How long to wait for the endpoint to bind before nagging the log. The
 * remote (dualcore_he) may come up well after the host if the two images
 * are flashed/reset independently, so this is a "still waiting" heartbeat,
 * not a hard failure.
 */
#define BOUND_WAIT_S 5

/*
 * Wire format shared with apps/dualcore_he/src/main.c -- both sides MUST
 * agree on this layout. It is duplicated rather than shared via a common
 * header because the two apps are independent Zephyr build systems (no
 * shared library between them by design of this project). Both cores are
 * Cortex-M55 (same endianness, same struct padding rules), so a raw memcpy
 * of this struct across the RPMsg link is safe.
 */
struct ping_pong_msg {
	uint32_t seq;
};

static K_SEM_DEFINE(bound_sem, 0, 1);
static K_SEM_DEFINE(pong_sem, 0, 1);

/* Written by ep_recv() (RPMsg backend's callback context), read by main()
 * only after pong_sem has been given -- the semaphore is what makes that
 * handoff safe, not any lock on these two globals.
 */
static struct ping_pong_msg pong_msg;
static uint32_t             pong_cycles;

static void ep_bound(void *priv)
{
	ARG_UNUSED(priv);
	k_sem_give(&bound_sem);
}

static void ep_recv(const void *data, size_t len, void *priv)
{
	ARG_UNUSED(priv);

	/* Capture the receive timestamp as close to arrival as possible, for
	 * an RTT measurement that isn't skewed by scheduler latency on the
	 * k_sem_take() side in main().
	 */
	pong_cycles = k_cycle_get_32();

	if (len != sizeof(pong_msg)) {
		LOG_ERR("PONG has unexpected length %zu (expected %zu); dropping", len, sizeof(pong_msg));
		return;
	}

	memcpy(&pong_msg, data, sizeof(pong_msg));
	k_sem_give(&pong_sem);
}

static struct ipc_ept_cfg ep_cfg = {
    .name = "dualcore_ping_pong",
    .cb =
        {
            .bound = ep_bound,
            .received = ep_recv,
        },
};

int main(void)
{
	const struct device *ipc0_instance;
	struct ipc_ept       ep;
	uint32_t             seq = 0;
	int                  ret;

	/* Banner: names this core and its console from the BUILD, never a
	 * literal -- CONFIG_BOARD_TARGET is Zephyr's own
	 * "<board>/<qualifiers>" string for whatever target this image was
	 * actually configured for, and CONSOLE_NODE_NAME is read off the
	 * `zephyr,console` chosen node above. A previous version of this
	 * banner hardcoded "rtss_hp" and "uart5 (E1M UART0)", which lied the
	 * moment this image was built for a different target.
	 */
	LOG_INF("=== Alp Lab E1M-AEN dualcore demo -- HOST ===");
	LOG_INF("board target: %s, console: %s", CONFIG_BOARD_TARGET, CONSOLE_NODE_NAME);

	/*
	 * Release the peer core via the Secure Enclave BEFORE opening ipc0.
	 * Do NOT abort on failure: log every distinct outcome class (local
	 * transport failure vs. SE-reported service error vs. success) and
	 * continue into the RPMsg host path regardless, so the demo still
	 * shows this side running even when the peer never comes up -- and
	 * says out loud why.
	 *
	 * alif_se_boot_cpu() alone (SE service_id 501, BOOT_CPU) is the call
	 * PROVEN on E1M-AEN801 silicon on 2026-07-30 -- see this file's
	 * top-of-file comment and docs/BENCH-DUALCORE.md. It does NOT run
	 * SET_VTOR first; alif_se_start_cpu() (SET_VTOR then BOOT_CPU) is an
	 * UNVERIFIED-ON-SILICON alternative this file deliberately does not
	 * use. CONFIG_DEMO_RELEASE_PEER_CPU_ID / CONFIG_DEMO_RELEASE_PEER_ENTRY
	 * (../Kconfig) name the peer and the entry address reported to the SE;
	 * on the bench-proven path the entry address is decorative (see
	 * ../Kconfig's help text) -- it is still passed because a future
	 * SES-ATOC-based release would use it.
	 */
	ret = alif_se_boot_cpu(CONFIG_DEMO_RELEASE_PEER_CPU_ID, CONFIG_DEMO_RELEASE_PEER_ENTRY);
	if (ret == 0) {
		LOG_INF("SE released peer cpu_id=%u, reported entry 0x%08x",
			CONFIG_DEMO_RELEASE_PEER_CPU_ID, CONFIG_DEMO_RELEASE_PEER_ENTRY);
	} else if (ret < 0) {
		LOG_ERR("alif_se_boot_cpu() local MHU transport failure (%d) -- peer cpu_id=%u "
		        "was NOT released; continuing as RPMsg host anyway, the peer will never "
		        "bind",
		        ret, CONFIG_DEMO_RELEASE_PEER_CPU_ID);
	} else {
		LOG_ERR("alif_se_boot_cpu() SE-reported service error (%d) -- peer cpu_id=%u was "
		        "NOT released; continuing as RPMsg host anyway, the peer will never bind",
		        ret, CONFIG_DEMO_RELEASE_PEER_CPU_ID);
	}

	ipc0_instance = DEVICE_DT_GET(DT_NODELABEL(ipc0));

	ret = ipc_service_open_instance(ipc0_instance);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("ipc_service_open_instance() failed: %d", ret);
		return ret;
	}

	ret = ipc_service_register_endpoint(ipc0_instance, &ep, &ep_cfg);
	if (ret < 0) {
		LOG_ERR("ipc_service_register_endpoint() failed: %d", ret);
		return ret;
	}

	/* The remote may not be up yet -- nag every BOUND_WAIT_S rather than
	 * blocking silently, so a demo watcher can tell "still booting" apart
	 * from "hung".
	 */
	while (k_sem_take(&bound_sem, K_SECONDS(BOUND_WAIT_S)) != 0) {
		LOG_WRN("endpoint not bound after %d s -- still waiting for the "
		        "rtss_he remote to register its endpoint",
		        BOUND_WAIT_S);
	}
	LOG_INF("endpoint bound; starting PING/PONG");

	for (;;) {
		struct ping_pong_msg ping = { .seq = seq };
		uint32_t             send_cycles;
		uint32_t             rtt_us;

		send_cycles = k_cycle_get_32();

		ret = ipc_service_send(&ep, &ping, sizeof(ping));
		if (ret < 0) {
			LOG_ERR("ipc_service_send() failed for PING seq=%u: %d", seq, ret);
			k_msleep(PING_PERIOD_MS);
			continue; /* retry the same seq rather than silently drop it */
		}

		/*
		 * Bounded wait, NOT K_FOREVER. A dropped PONG is reachable in normal
		 * operation: the rtss_he remote logs and discards the echo when its own
		 * ipc_service_send() fails (see apps/dualcore_he/src/main.c). With
		 * K_FOREVER a single such drop parks this thread permanently -- the demo
		 * goes silent with no diagnostic, which is the worst failure mode to hit
		 * in front of a customer. Two PING periods is generous: a doorbell round
		 * trip between two M55s on the same die is microseconds, not
		 * milliseconds.
		 */
		if (k_sem_take(&pong_sem, K_MSEC(PING_PERIOD_MS * 2)) != 0) {
			LOG_ERR("no PONG for seq=%u within %d ms -- dropping it and "
			        "continuing with the next sequence number",
			        seq, PING_PERIOD_MS * 2);
			seq++;
			k_msleep(PING_PERIOD_MS);
			continue;
		}

		if (pong_msg.seq != seq) {
			LOG_WRN("PONG seq mismatch: sent %u, got %u", seq, pong_msg.seq);
		}

		rtt_us = k_cyc_to_us_floor32((uint32_t)(pong_cycles - send_cycles));
		LOG_INF("PONG seq=%u rtt=%u us", pong_msg.seq, rtt_us);

		seq++;
		k_msleep(PING_PERIOD_MS);
	}
}
