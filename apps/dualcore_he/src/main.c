/*
 * Copyright 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * dualcore_he -- IPC-service RPMsg REMOTE demo, rtss_he cluster of the Alp
 * Lab E1M-AEN SoM (Alif Ensemble E4/E8, ae402fa0e5597le0 / ae822fa0e5597ls0).
 *
 * WHAT THIS DEMONSTRATES: the remote side of the dual-core RPMsg link built
 * in apps/dualcore_hp -- see that file's top-of-file comment for the full
 * picture (backend, mailbox driver, MRAM/board layout). This file is
 * intentionally the smaller half: it has no notion of demo cadence, it just
 * answers whatever the host sends.
 *
 * ROLE: this cluster is the RPMsg "remote" -- see the ipc0 node's
 * role = "remote" property in boards/e1m_aen_*_rtss_he.overlay. Its
 * counterpart is apps/dualcore_hp, the "host", running on the SAME
 * silicon's rtss_hp cluster out of the other half of the split MRAM.
 *
 * PROTOCOL: on every received struct ping_pong_msg (a PING), this thread
 * logs it and echoes the identical struct straight back as the PONG -- no
 * sequence-number bookkeeping of its own; the host owns the sequence and
 * detects gaps/reordering on its side.
 *
 * WHY THE SEND HAPPENS IN main()'S THREAD, NOT IN THE RECEIVED CALLBACK:
 * the RPMsg backend invokes .received from its own processing context, and
 * every upstream ipc_service sample (see
 * zephyr/samples/subsys/ipc/ipc_service/BACKEND/remote/src/main.c) defers the
 * reply to a separate thread woken by a semaphore rather than calling
 * ipc_service_send() straight out of that callback. This file follows the
 * same pattern.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(dualcore_he, LOG_LEVEL_INF);

/* Console node name (e.g. "uart@4901a000"), read straight off the
 * `zephyr,console` chosen node this board target picked -- NEVER a literal
 * UART/core name in this file. A previous version of this banner hardcoded
 * "uart3 (E1M UART1)", which is wrong the moment this image is built for a
 * different board target -- see docs/BENCH-DUALCORE.md for a concrete case:
 * on the bench-proven dual-core run this app's REMOTE logic executed on
 * RTSS-HP, not RTSS-HE.
 */
#define CONSOLE_NODE_NAME DT_NODE_FULL_NAME(DT_CHOSEN(zephyr_console))

/* How long to wait for the endpoint to bind before nagging the log. See
 * dualcore_hp/src/main.c for the identical rationale.
 */
#define BOUND_WAIT_S 5

/*
 * Wire format shared with apps/dualcore_hp/src/main.c -- both sides MUST
 * agree on this layout; see that file's comment for why it is duplicated
 * rather than shared via a common header.
 */
struct ping_pong_msg {
	uint32_t seq;
};

static K_SEM_DEFINE(bound_sem, 0, 1);
static K_SEM_DEFINE(ping_sem, 0, 1);

/* Written by ep_recv() (RPMsg backend's callback context), read by main()
 * only after ping_sem has been given.
 */
static struct ping_pong_msg ping_msg;

static void ep_bound(void *priv)
{
	ARG_UNUSED(priv);
	k_sem_give(&bound_sem);
}

static void ep_recv(const void *data, size_t len, void *priv)
{
	ARG_UNUSED(priv);

	if (len != sizeof(ping_msg)) {
		LOG_ERR("PING has unexpected length %zu (expected %zu); dropping", len, sizeof(ping_msg));
		return;
	}

	memcpy(&ping_msg, data, sizeof(ping_msg));
	k_sem_give(&ping_sem);
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
	int                  ret;

	/* Banner: names this core and its console from the BUILD, never a
	 * literal -- CONFIG_BOARD_TARGET is Zephyr's own
	 * "<board>/<qualifiers>" string for whatever target this image was
	 * actually configured for, and CONSOLE_NODE_NAME is read off the
	 * `zephyr,console` chosen node above. A previous version of this
	 * banner hardcoded "rtss_he" and "uart3 (E1M UART1)", which lied the
	 * moment this image was built for (or run on, via a debugger, as on
	 * the bench-proven path -- see docs/BENCH-DUALCORE.md) a different
	 * target.
	 */
	LOG_INF("=== Alp Lab E1M-AEN dualcore demo -- REMOTE ===");
	LOG_INF("board target: %s, console: %s", CONFIG_BOARD_TARGET, CONSOLE_NODE_NAME);

	ipc0_instance = DEVICE_DT_GET(DT_NODELABEL(ipc0));

	/*
	 * Log BEFORE opening, because on the remote side this call does not
	 * return until the host is up. The RPMsg static-vrings backend runs
	 * ipc_rpmsg_init() in this thread; for a non-host role that reaches
	 * rpmsg_init_vdev() -> rpmsg_virtio_wait_remote_ready(), an infinite
	 * loop polling the shared-memory virtio status byte for DRIVER_OK and
	 * k_sleep()ing 1 ms per pass. Only the rtss_hp host ever writes
	 * DRIVER_OK. So with no host running, this app parks here forever --
	 * correct standalone behaviour, but indistinguishable from a hang
	 * unless we say so first. Confirmed on E1M-AEN801 silicon: the board
	 * printed the two banner lines and then nothing for 45 s, with the
	 * core healthy in the idle thread (IPSR=0, no fault, no bound-wait
	 * warnings -- because the nag loop further down is never reached).
	 */
	LOG_INF("opening ipc0 as RPMsg remote -- BLOCKS here until the rtss_hp "
	        "host signals virtio DRIVER_OK in shared memory");

	ret = ipc_service_open_instance(ipc0_instance);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("ipc_service_open_instance() failed: %d", ret);
		return ret;
	}

	LOG_INF("ipc0 open -- host is alive");

	ret = ipc_service_register_endpoint(ipc0_instance, &ep, &ep_cfg);
	if (ret < 0) {
		LOG_ERR("ipc_service_register_endpoint() failed: %d", ret);
		return ret;
	}

	/* The host may not have registered its endpoint yet -- nag every
	 * BOUND_WAIT_S rather than blocking silently.
	 */
	while (k_sem_take(&bound_sem, K_SECONDS(BOUND_WAIT_S)) != 0) {
		LOG_WRN("endpoint not bound after %d s -- still waiting for the "
		        "rtss_hp host to register its endpoint",
		        BOUND_WAIT_S);
	}
	LOG_INF("endpoint bound; waiting for PING");

	for (;;) {
		k_sem_take(&ping_sem, K_FOREVER);

		LOG_INF("PING seq=%u received; echoing PONG", ping_msg.seq);

		ret = ipc_service_send(&ep, &ping_msg, sizeof(ping_msg));
		if (ret < 0) {
			LOG_ERR("ipc_service_send() failed for PONG seq=%u: %d", ping_msg.seq, ret);
		}
	}
}
