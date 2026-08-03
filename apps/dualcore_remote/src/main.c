/*
 * Copyright 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * dualcore_remote -- IPC-service RPMsg REMOTE demo for the Alp Lab E1M-AEN
 * SoM (Alif Ensemble E4/E8, ae402fa0e5597le0 / ae822fa0e5597ls0).
 *
 * WHAT THIS DEMONSTRATES: the remote side of the dual-core RPMsg link built
 * in apps/dualcore_host -- see that file's top-of-file comment for the full
 * picture (backend, mailbox driver, MRAM/board layout). This file is
 * intentionally the smaller half: it has no notion of demo cadence, it just
 * answers whatever the host sends.
 *
 * ROLE: this app is the RPMsg "remote" on WHICHEVER M55 cluster it is built
 * for -- see the ipc0 node's role = "remote" property in every
 * boards/e1m_aen_*.overlay in this app. Named by role, not by cluster, for
 * the same reason apps/dualcore_host is -- see that file's ROLE comment.
 * This app builds for both the rtss_hp and rtss_he qualifiers of both SoCs.
 * Its counterpart is apps/dualcore_host, the "host", running on the SAME
 * silicon's OTHER cluster out of the other half of the split MRAM.
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

#ifdef CONFIG_DEMO_REMOTE_BEACON
#include <zephyr/cache.h>
#include <zephyr/init.h>
#endif

LOG_MODULE_REGISTER(dualcore_remote, LOG_LEVEL_INF);

#ifdef CONFIG_DEMO_REMOTE_BEACON
/*
 * --- Liveness beacon (bench diagnostic, NOT shipped) -----------------------
 *
 * Off by default (CONFIG_DEMO_REMOTE_BEACON, see ../Kconfig) -- enabled only
 * with `-DEXTRA_CONF_FILE=beacon.conf` (../beacon.conf), the same opt-in
 * shape dualcore_host's breadcrumb.conf uses. The shipped default image
 * carries none of this.
 *
 * WHY: dualcore_host's console can prove it completed the SE core-release
 * call and that the release entry address (this app's own reset vector)
 * already held plausible-looking code before release -- but it has no way to
 * observe anything on THIS side of that release. This core's own DTCM/ITCM
 * is not reachable from a debug-AP session attached to the host's cluster,
 * so "released but the host never sees a PONG" is consistent with several
 * different failures that all look identical from the host alone: this core
 * never executed; it executed and hung before opening ipc0; it opened ipc0
 * fine but the endpoint bind itself never completes (e.g. because the shared
 * vring memory is cache-incoherent between the two M55 D-caches). This
 * beacon writes to global SRAM0 -- physical memory reachable from EITHER
 * core's debug-AP, unlike this core's own local RAM -- so a bench read can
 * tell those apart without this core's console (which may itself depend on
 * the very thing that is broken) and without a debugger halting this core.
 *
 *   0x02000100 = BEACON_MAGIC, written by the SYS_INIT(EARLY, 0) hook below
 *     -- the earliest C code this build can reach without hand-writing
 *     assembly (see beacon_mark_alive()'s own comment for exactly what
 *     already ran before it). Presence alone proves this core executed at
 *     all -- independent of everything below it.
 *   0x02000104 = BEACON_CORE_ID, written by the same SYS_INIT hook. NOT an
 *     ARM CPUID register read: Cortex-M55 SCB->CPUID reads the same PARTNO
 *     on both RTSS-HP and RTSS-HE (they are the same core RTL instantiated
 *     twice), so it cannot distinguish which cluster wrote a given beacon --
 *     the one thing this word exists to do. Instead this is a
 *     build-time-selected SE-domain CPU-id value (2 = M55_HP, 3 = M55_HE),
 *     the same numbering modules/alif-se-boot/include/alif_se_boot.h's
 *     ALIF_SE_BOOT_CPU_M55_HP/_HE constants use and dualcore_host's Kconfig
 *     (CONFIG_DEMO_RELEASE_PEER_CPU_ID) already reports to the SE for this
 *     same release -- not duplicated by #include, since this app has no
 *     dependency on modules/alif-se-boot (host-only, see ../dualcore_host/
 *     prj.conf), the same reason struct ping_pong_msg below is duplicated
 *     rather than shared.
 *   0x02000108 = BEACON_HEARTBEAT, incremented every
 *     BEACON_HEARTBEAT_PERIOD_MS by a dedicated auto-started thread (see
 *     beacon_heartbeat_thread_entry() below), independent of ipc0's own
 *     state. A single read only proves this core ran ONCE; a re-read some
 *     seconds later that shows this word still advancing is what proves the
 *     core is still alive right now, as opposed to having written the first
 *     three words and then hung or faulted.
 *   0x0200010C = BEACON_PHASE, updated at each RPMsg bring-up milestone in
 *     main() below: BEACON_PHASE_OPEN_DONE after ipc_service_open_instance()
 *     returns, BEACON_PHASE_REGISTER_DONE after
 *     ipc_service_register_endpoint() returns, BEACON_PHASE_BOUND when the
 *     endpoint's own .bound callback fires (ep_bound() below). Read together
 *     with the heartbeat, this is what answers WHERE in the bring-up a stuck
 *     remote actually stopped -- "still alive but stuck between
 *     OPEN_DONE and REGISTER_DONE" and "dead before ever reaching OPEN_DONE"
 *     are indistinguishable from the host's side alone, and look identical
 *     to a "no PONG" timeout either way.
 *   0x02000110 = BEACON_RC_OPEN, the verbatim int return value of
 *     ipc_service_open_instance(), written in the same store as
 *     BEACON_PHASE_OPEN_DONE.
 *   0x02000114 = BEACON_RC_REGISTER, the verbatim int return value of
 *     ipc_service_register_endpoint(), written in the same store as
 *     BEACON_PHASE_REGISTER_DONE.
 *
 * PLACEMENT: 0x02000100-0x02000114 is a DIFFERENT address range from
 * dualcore_host's own execution-breadcrumb block (0x02000000-0x02000038,
 * see ../dualcore_host/Kconfig's CONFIG_DEMO_EXECUTION_BREADCRUMB and
 * ../dualcore_host/src/main.c) -- deliberately, so the two apps' bench
 * diagnostics can never be confused on a read of global SRAM0, even with
 * both enabled at once (host on one cluster, remote on the other, sharing
 * the same physical SRAM0 -- see soc's sram0@2000000, 4 MB). Both ranges sit
 * well below the sram_ipc0 carve-out at 0x02010000 (see the board .dts), so
 * neither can ever collide with the OpenAMP vrings/payload either.
 *
 * CACHE: this SoC family defaults CONFIG_CACHE_MANAGEMENT=y
 * (soc/alif/ensemble/Kconfig.defconfig) and soc_reset_hook() enables the
 * D-cache before any C code in this file runs, so a plain store to global
 * SRAM0 can sit in the D-cache and never reach the physical memory an
 * external SWD probe reads -- the exact defect dualcore_host's own
 * breadcrumb comment describes hunting down on the SE request buffer.
 * Every beacon write below is followed by sys_cache_data_flush_range() for
 * exactly that reason.
 */
#define BEACON_ADDR_MAGIC        ((volatile uint32_t *)0x02000100u)
#define BEACON_ADDR_CORE_ID      ((volatile uint32_t *)0x02000104u)
#define BEACON_ADDR_HEARTBEAT    ((volatile uint32_t *)0x02000108u)
#define BEACON_ADDR_PHASE        ((volatile uint32_t *)0x0200010Cu)
#define BEACON_ADDR_RC_OPEN      ((volatile uint32_t *)0x02000110u)
#define BEACON_ADDR_RC_REGISTER  ((volatile uint32_t *)0x02000114u)

/* Not 0, 0xFFFFFFFF, or a plausible leftover-SRAM value -- unmistakable in a
 * raw hex dump. Deliberately a different bit pattern from dualcore_host's
 * own 0x5A5AxxxxA5-family magics (see that app's src/main.c) so the two
 * apps' beacons are visually distinct too, not just address-disjoint.
 */
#define BEACON_MAGIC 0xC3C33C3Cu

/* SE-domain CPU id, per modules/alif-se-boot/include/alif_se_boot.h's
 * ALIF_SE_BOOT_CPU_M55_HP/_HE constants (2 / 3) -- see the top-of-file
 * comment above for why this, and not an ARM CPUID register read, is what
 * belongs at BEACON_ADDR_CORE_ID. Selected at compile time from the SOC
 * Kconfig symbol this exact board target selects (soc/alif/ensemble/{e4,e8}/
 * Kconfig.soc), covering both SoCs this app builds for.
 */
#if defined(CONFIG_SOC_AE822FA0E5597LS0_RTSS_HP) || defined(CONFIG_SOC_AE402FA0E5597LE0_RTSS_HP)
#define BEACON_CORE_ID 0x00000002u
#elif defined(CONFIG_SOC_AE822FA0E5597LS0_RTSS_HE) || defined(CONFIG_SOC_AE402FA0E5597LE0_RTSS_HE)
#define BEACON_CORE_ID 0x00000003u
#else
#error "unknown RTSS core qualifier -- add it to BEACON_CORE_ID's selection in src/main.c"
#endif

#define BEACON_PHASE_OPEN_DONE     1u
#define BEACON_PHASE_REGISTER_DONE 2u
#define BEACON_PHASE_BOUND         3u

#define BEACON_HEARTBEAT_PERIOD_MS 250
#define BEACON_HEARTBEAT_STACK_SIZE 512
#define BEACON_HEARTBEAT_THREAD_PRIO 7

static void beacon_write(volatile uint32_t *addr, uint32_t value)
{
	*addr = value;
	sys_cache_data_flush_range((void *)addr, sizeof(*addr));
}

/*
 * SYS_INIT(..., EARLY, 0) is the earliest hook this app can reach without
 * hand-writing assembly -- same rationale as dualcore_host's
 * breadcrumb_mark_early() (see that file's comment above its own SYS_INIT
 * call for exactly what already ran before this point on this SoC).
 */
static int beacon_mark_alive(void)
{
	beacon_write(BEACON_ADDR_MAGIC, BEACON_MAGIC);
	beacon_write(BEACON_ADDR_CORE_ID, BEACON_CORE_ID);
	return 0;
}
SYS_INIT(beacon_mark_alive, EARLY, 0);

/*
 * Heartbeat -- a dedicated thread, not a write in main()'s own loop, so it
 * keeps advancing even while main() is blocked inside
 * ipc_service_open_instance() (which does not return on this, the REMOTE,
 * side until the host is up -- see main()'s own comment below) or stuck
 * anywhere else. K_THREAD_DEFINE starts this automatically at kernel init,
 * with no explicit spawn call needed in main().
 */
static void beacon_heartbeat_thread_entry(void *p1, void *p2, void *p3)
{
	uint32_t heartbeat = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		beacon_write(BEACON_ADDR_HEARTBEAT, heartbeat);
		heartbeat++;
		k_msleep(BEACON_HEARTBEAT_PERIOD_MS);
	}
}
K_THREAD_DEFINE(beacon_heartbeat_tid, BEACON_HEARTBEAT_STACK_SIZE, beacon_heartbeat_thread_entry,
		 NULL, NULL, NULL, K_PRIO_PREEMPT(BEACON_HEARTBEAT_THREAD_PRIO), 0, 0);
#endif /* CONFIG_DEMO_REMOTE_BEACON */

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
 * dualcore_host/src/main.c for the identical rationale.
 */
#define BOUND_WAIT_S 5

/*
 * Wire format shared with apps/dualcore_host/src/main.c -- both sides MUST
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
#ifdef CONFIG_DEMO_REMOTE_BEACON
	/* Runs in the RPMsg backend's own callback context, same as ep_recv()
	 * below -- a plain store + cache flush needs no lock either way. See
	 * the top-of-file beacon comment for what BEACON_PHASE_BOUND means.
	 */
	beacon_write(BEACON_ADDR_PHASE, BEACON_PHASE_BOUND);
#endif
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
	 * k_sleep()ing 1 ms per pass. Only the host ever writes DRIVER_OK. So
	 * with no host running, this app parks here forever --
	 * correct standalone behaviour, but indistinguishable from a hang
	 * unless we say so first. Confirmed on E1M-AEN801 silicon: the board
	 * printed the two banner lines and then nothing for 45 s, with the
	 * core healthy in the idle thread (IPSR=0, no fault, no bound-wait
	 * warnings -- because the nag loop further down is never reached).
	 */
	LOG_INF("opening ipc0 as RPMsg remote -- BLOCKS here until the "
	        "host signals virtio DRIVER_OK in shared memory");

	ret = ipc_service_open_instance(ipc0_instance);
#ifdef CONFIG_DEMO_REMOTE_BEACON
	/* See the top-of-file beacon comment for why phase and return code
	 * are written together, immediately after the call returns -- this is
	 * the FIRST thing on this path after the call, before the LOG_ERR
	 * error-handling below, so it captures the outcome even on the error
	 * branch.
	 */
	beacon_write(BEACON_ADDR_RC_OPEN, (uint32_t)ret);
	beacon_write(BEACON_ADDR_PHASE, BEACON_PHASE_OPEN_DONE);
#endif
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("ipc_service_open_instance() failed: %d", ret);
		return ret;
	}

	LOG_INF("ipc0 open -- host is alive");

	ret = ipc_service_register_endpoint(ipc0_instance, &ep, &ep_cfg);
#ifdef CONFIG_DEMO_REMOTE_BEACON
	beacon_write(BEACON_ADDR_RC_REGISTER, (uint32_t)ret);
	beacon_write(BEACON_ADDR_PHASE, BEACON_PHASE_REGISTER_DONE);
#endif
	if (ret < 0) {
		LOG_ERR("ipc_service_register_endpoint() failed: %d", ret);
		return ret;
	}

	/* The host may not have registered its endpoint yet -- nag every
	 * BOUND_WAIT_S rather than blocking silently.
	 */
	while (k_sem_take(&bound_sem, K_SECONDS(BOUND_WAIT_S)) != 0) {
		LOG_WRN("endpoint not bound after %d s -- still waiting for the "
		        "host to register its endpoint",
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
