/*
 * Copyright 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * dualcore_host -- IPC-service RPMsg HOST demo for the Alp Lab E1M-AEN SoM
 * (Alif Ensemble E4/E8, ae402fa0e5597le0 / ae822fa0e5597ls0).
 *
 * WHAT THIS DEMONSTRATES: a minimal, from-scratch dual-core RPMsg link built
 * entirely on upstream Zephyr (ipc_service + the OpenAMP/RPMsg static-vrings
 * backend) plus one small out-of-tree mailbox driver (modules/alif-mhuv2,
 * CONFIG_MBOX_ALIF_MHUV2) for the Alif MHUv2 doorbell hardware that backend
 * needs to signal the other core. Nothing here depends on alp-sdk.
 *
 * ROLE: this app is the RPMsg "host" on WHICHEVER M55 cluster it is built
 * for -- see the ipc0 node's role = "host" property in every
 * boards/e1m_aen_*.overlay in this app. Named by role, not by cluster,
 * because on this silicon the cluster the Secure Enclave boots first (and
 * so the one that must run the host, since it is the one with SE access)
 * is NOT always rtss_hp -- see docs/BENCH-DUALCORE.md section 0. This app
 * builds for both the rtss_hp and rtss_he qualifiers of both SoCs; which one
 * matches a given board's actual boot order is a per-SoC fact recorded in
 * docs/BENCH-DUALCORE.md and scripts/build-all.sh, not in this file. Its
 * counterpart is apps/dualcore_remote, the "remote", running on the SAME
 * silicon's OTHER cluster out of the other half of the split MRAM (see the
 * board .dts comments for the partition layout). Both images must be
 * flashed together; neither one boots the whole board's peripherals -- each
 * M55 cluster is an independent Zephyr instance that only knows about its
 * own core.
 *
 * PROTOCOL: every PING_PERIOD_MS this thread sends a struct ping_pong_msg
 * carrying a monotonically increasing sequence number, then blocks for the
 * matching PONG. The remote echoes every PING back verbatim (see
 * apps/dualcore_remote/src/main.c). Round-trip time is measured with the
 * core's own cycle counter (k_cycle_get_32/k_cyc_to_us_floor32) captured at
 * send and at receive -- both timestamps are taken on THIS core, so there is
 * no cross-core clock to synchronize.
 *
 * HOW TO RUN: flash this image to e1m_aen/<soc>/<qualifier> and
 * dualcore_remote's image to e1m_aen/<soc>/<the other qualifier> (same
 * <soc>), then watch two serial terminals -- the console for each is
 * whatever `zephyr,console` names for that board target (see the root
 * README.md's console-assignment table; the E1M carrier pinout for these
 * UARTs is NOT yet confirmed against real hardware).
 *
 * SECURE-ENCLAVE BOOT: this core is the one with SE access, so it is also
 * the one that asks the Secure Enclave to release its peer core before the
 * RPMsg link has any chance of working -- see the alif_se_start_cpu() call
 * near the top of main(), modules/alif-se-boot for the client, and
 * docs/BENCH-DUALCORE.md for the sequence run on E1M-AEN801 silicon. The
 * peer's SE-domain CPU id and reported entry address are Kconfig options
 * (CONFIG_DEMO_RELEASE_PEER_CPU_ID / CONFIG_DEMO_RELEASE_PEER_ENTRY, see
 * ../Kconfig), not literals in this file -- their defaults encode the
 * bench-established shape (release M55-HP, entry 0x50000000), which is the
 * MIRROR of what this file used to hardcode (release M55-HE at
 * 0x58000000).
 *
 * This file calls alif_se_start_cpu() (modules/alif-se-boot), the
 * SET_VTOR -> RESET_CPU -> RELEASE_CPU sequence -- not alif_se_boot_cpu()
 * (SE service_id 501, BOOT_CPU) alone, which this file called on an earlier
 * revision that a bench run PROVED wrong: the released core came up with
 * VTOR == 0x00000000 and fetched its initial SP/PC from its own LOCAL
 * address 0 regardless of the entry address reported to BOOT_CPU, because
 * BOOT_CPU never transfers a vector table base into the core's own VTOR
 * register -- only RESET_CPU does that, and only after SET_VTOR has staged
 * the value. See modules/alif-se-boot/include/alif_se_boot.h's
 * alif_se_start_cpu() doc comment for the Alif DFP documentation this
 * ordering rests on. UNVERIFIED ON SILICON in this exact three-call form --
 * see modules/alif-se-boot/README.md.
 *
 * Ordering is deliberate: ipc_service_open_instance() on the HOST side
 * (this core) does not block waiting for the remote -- only the REMOTE
 * blocks in its own open call, waiting for this core's virtio DRIVER_OK.
 * So releasing the peer first, then opening as host, is safe either way:
 * if the release fails, this core still proceeds to open ipc0 and shows
 * this side of the demo running, with a clear log line explaining why the
 * peer never binds an endpoint.
 */

#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/fatal.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/time_units.h>

#ifdef CONFIG_DEMO_EXECUTION_BREADCRUMB
#include <zephyr/cache.h>
#include <zephyr/init.h>
#endif

#include <alif_se_boot.h>

LOG_MODULE_REGISTER(dualcore_host, LOG_LEVEL_INF);

#ifdef CONFIG_DEMO_EXECUTION_BREADCRUMB
/*
 * --- Execution breadcrumbs (bench diagnostic, NOT shipped) -----------------
 *
 * Off by default (CONFIG_DEMO_EXECUTION_BREADCRUMB, see ../Kconfig) --
 * enabled only with `-DEXTRA_CONF_FILE=breadcrumb.conf` (../breadcrumb.conf),
 * the same opt-in shape ../ram-console.conf uses. The shipped default image
 * carries none of this.
 *
 * WHY: on silicon this image comes up with VTOR_S == 0 and faults, while the
 * canonical person_detect image at the same MRAM address on the same core
 * comes up with VTOR_S == 0x80010000 and runs. Neither the MRAM address nor
 * the ATOC shape is the difference -- both were tested to destruction. The
 * one thing this image does that person_detect never does is talk to the SE
 * (the alif_se_start_cpu() release sequence below, releasing the peer
 * core) -- SERVICES_power_m55_he_vtor_save() exists in hal_alif precisely
 * because a core re-released after a power transition can come back with
 * its SE-side VTOR unrestored. So the open question is not "why does VTOR_S
 * read 0" but "did this core ever run, and if so, how far did it get before
 * that sequence". These words answer that from a hex dump alone -- no
 * debugger halting the core, no source tree in front of the reader:
 *
 *   0x02000000 = BREADCRUMB_MAGIC_EARLY (0x5A5AA5A5), written by the
 *     SYS_INIT(EARLY, 0) hook below -- the earliest C code this build can
 *     reach without hand-writing assembly (see the hook's own comment for
 *     exactly what already ran before it).
 *   0x02000004 = BREADCRUMB_MAGIC_PRE_RELEASE (0xA5A55A5A), written in
 *     main() immediately before the release sequence (alif_se_start_cpu(),
 *     or -- see the "per-step" section below -- its decomposed
 *     SET_VTOR/RESET_CPU/RELEASE_CPU equivalent).
 *   0x02000008 = BREADCRUMB_MAGIC_POST_RELEASE (0x5A5A5A5A), written in
 *     main() as the FIRST thing after the release sequence returns -- before
 *     any LOG_* call or other code on that path. Its presence alone answers
 *     "did the sequence return at all", independent of what it returned.
 *   0x0200000C = the verbatim int return value of the release sequence
 *     (alif_se_start_cpu(), or the equivalent overall result of its
 *     decomposed form), written immediately after the 0x02000008 store (same
 *     store sequence, still before any LOG_*). Not a magic -- read it as a
 *     signed 32-bit value per alif_se_boot.h's shared retval contract: 0 =
 *     SE-confirmed success, <0 = a LOCAL MHU transport failure (no SE-side
 *     outcome known), >0 = an SE-REPORTED service error (resp_error_code, or
 *     header.error_code if that is the only nonzero field -- see
 *     alif_se_boot_cpu()'s doc comment, whose retval contract every function
 *     in this module shares). None of these functions expose the two
 *     SE-side fields separately through their public API
 *     (modules/alif-se-boot/src/alif_se_boot.c collapses them into this
 *     single return value before returning; the raw struct is not reachable
 *     from outside that module) -- so this word IS the most granular
 *     SE-side answer for the OVERALL sequence this diagnostic can get
 *     without changing that module's API, which is out of scope here (see
 *     the per-step words below for which INDIVIDUAL call this came from).
 *
 * Reading them on the bench:
 *   neither 0x02000000 nor    -> this core never executed at all; the SES
 *   0x02000004 present           withheld the vector base for a reason
 *                                 upstream of this image entirely.
 *   0x02000000 only              -> it ran, but died or hung before reaching
 *                                 the release sequence -- look downstream of
 *                                 SYS_INIT(EARLY) and upstream of main().
 *   0x02000000 and 0x02000004    -> it ran and reached the release
 *   present, 0x02000008 ABSENT      sequence, but nothing in it ever
 *                                 returned -- the call takes the core down,
 *                                 or the SE resets the caller. Look at the
 *                                 release parameters (cpu_id/entry), the
 *                                 per-step words below for which call that
 *                                 was, or the MHU transport sequence.
 *   all of 0x02000000,           -> the sequence RETURNED; 0x0200000C says
 *   0x02000004, 0x02000008          whether the SE accepted (0) or rejected
 *   present                          (>0) it overall, or whether it was a
 *                                 local transport failure (<0) -- see the
 *                                 per-step words below for which step that
 *                                 outcome belongs to. Either way the core
 *                                 came down AFTER the sequence returned --
 *                                 the damage is downstream of it, not in the
 *                                 sequence itself.
 *   all four present,             -> not this bug; look elsewhere.
 *   VTOR_S != 0
 *
 * No magic here is 0, 0xFFFFFFFF, or a value that could pass for leftover
 * SRAM noise -- all three are the same classic 5/A bit-test family
 * (0x5A5AA5A5 / 0xA5A55A5A / 0x5A5A5A5A), unmistakable against surrounding
 * zeroed or garbage memory in a raw hex dump, and pairwise distinct from each
 * other.
 *
 * --- SE-MHU identity probe (0x02000010 - 0x0200002C) -----------------------
 *
 * WHY: modules/alif-se-boot/src/alif_se_boot.c's release-sequence calls
 * below talk to a SEPARATE, SE-dedicated ARM MHUv2 frame pair (se_mhu_tx /
 * se_mhu_rx -- NOT the mhu_tx/mhu_rx pair the RPMsg link uses), and that
 * module's own file header says which cluster has live SE access is a
 * per-silicon boot-order fact, not always this one. A plausible-looking
 * `rc = 0` from those calls proves nothing about whether THIS core was
 * ever actually talking to a live MHU at that address -- writes could
 * just as well be landing in unmapped or differently-decoded space and
 * reading back stale/garbage state, with a core reset following. These eight
 * words answer that independently of whether the release sequence is even
 * reached, by reading each frame's own CoreSight identification registers
 * (offsets 0xFE0-0xFFC from the frame base) BEFORE 0x02000004 is written:
 *
 *   0x02000010 = se_mhu_tx PID0   0x02000020 = se_mhu_rx PID0
 *   0x02000014 = se_mhu_tx CID0   0x02000024 = se_mhu_rx CID0
 *   0x02000018 = se_mhu_tx PID1   0x02000028 = se_mhu_rx PID1
 *   0x0200001C = se_mhu_tx CID1   0x0200002C = se_mhu_rx CID1
 *
 * An ARM MHUv2 block reads PID0 == 0x76, CID0 == 0x0D (the standard ARM
 * CoreSight preamble byte) at every frame this IP exposes -- those two are
 * the ones that matter. PID1/CID1 are captured too only because the extra
 * two reads cost nothing once the frame is already being read; they are not
 * compared against an expected value below. Anything else at PID0/CID0 --
 * zero, all-ones, or any other value -- means THIS core is not looking at an
 * MHUv2 block at that address at this point in boot, independent of whatever
 * the release sequence itself later reports.
 *
 * Base addresses come from DT_REG_ADDR(DT_NODELABEL(se_mhu_tx)) /
 * DT_NODELABEL(se_mhu_rx) below -- the SAME devicetree nodes
 * modules/alif-se-boot/src/alif_se_boot.c reaches (by phandle, as
 * se_boot_tx_base()/se_boot_rx_base()) -- never a second 0x40050000 /
 * 0x40040000 literal in this file.
 *
 * FAULT TOLERANCE: reading an address with no live peripheral behind it can
 * bus-fault, exactly like the pre-release ITCM probe below -- see that
 * probe's own comment and k_sys_fatal_error_handler() further down for the
 * shared "disposable thread, bounded wait, abort only that thread" pattern
 * this probe reuses. Each register is breadcrumb-written (with its own
 * cache flush) immediately after it is read, so a fault partway through this
 * sequence still leaves every earlier register's result readable -- absence
 * of a given word means that specific read never completed, not that every
 * later one didn't either.
 *
 * PLACEMENT: every breadcrumb address in this file (0x02000000 through
 * 0x02000038) is inside sram0@2000000 (global SRAM0, 4 MB --
 * zephyr/dts/arm/alif/ensemble/common/ensemble_rtss_he.dtsi), NOT this app's
 * zephyr,sram (&dtcm, local 0x20000000 -- see the board .dts), so
 * arch_bss_zero() never touches it; a breadcrumb written here survives this
 * app's own C runtime init. The whole range also sits well below the
 * sram_ipc0 carve-out at 0x02010000 (see the board .dts), so it can never
 * collide with the OpenAMP vrings/payload.
 *
 * CACHE: this SoC family defaults CONFIG_CACHE_MANAGEMENT=y
 * (soc/alif/ensemble/Kconfig.defconfig) and soc_reset_hook() enables the
 * D-cache before any C code in this file runs -- so a plain store to global
 * SRAM0 can sit in the D-cache and never reach the physical memory an
 * external SWD probe reads. Every write below is followed by
 * sys_cache_data_flush_range() for exactly that reason; skipping it would
 * make an absent breadcrumb misread as "never ran" when it actually means
 * "ran, but the write was still only in cache".
 *
 * --- Per-step SE release-sequence breadcrumbs (0x02000030 - 0x02000038) ---
 *
 * WHY: alif_se_start_cpu() (modules/alif-se-boot) is now three separate SE
 * service calls -- SET_VTOR, RESET_CPU, RELEASE_CPU, each gated behind its
 * own readiness heartbeat -- stopping at the first that fails. The single
 * PRE_RELEASE/POST_RELEASE/RETVAL breadcrumbs above only bracket the whole
 * sequence: they say the sequence started and, if 0x02000008 is present,
 * that SOME call in it returned -- not which of the three that was. These
 * three words answer that, from a hex dump alone, when
 * CONFIG_DEMO_EXECUTION_BREADCRUMB is on: this build decomposes the
 * sequence into its three underlying calls (alif_se_set_vtor() /
 * alif_se_reset_cpu() / alif_se_release_cpu(), the same three functions and
 * the same order alif_se_start_cpu() itself uses -- see that function's doc
 * comment in include/alif_se_boot.h) so a breadcrumb can be written after
 * each one returns, instead of only after the whole sequence does. A build
 * with CONFIG_DEMO_EXECUTION_BREADCRUMB off calls alif_se_start_cpu()
 * directly, as a single opaque call -- see main()'s SECURE-ENCLAVE BOOT
 * step below for both forms.
 *
 *   0x02000030 = alif_se_set_vtor()'s verbatim int return value, written
 *     immediately after it returns. Absence means it never returned (the
 *     call hung, or the core went down before this write's cache flush
 *     completed).
 *   0x02000034 = alif_se_reset_cpu()'s verbatim int return value, written
 *     immediately after it returns -- present ONLY if 0x02000030 was 0 (the
 *     sequence stops at the first failing step, matching
 *     alif_se_start_cpu()'s own contract). Absent with 0x02000030 present
 *     and 0 means: SET_VTOR succeeded, but RESET_CPU never returned.
 *   0x02000038 = alif_se_release_cpu()'s verbatim int return value, written
 *     immediately after it returns -- present ONLY if both 0x02000030 and
 *     0x02000034 were 0. This word being 0 is the strongest evidence this
 *     diagnostic can produce that the SE actually ran all three services
 *     and reported success on each.
 *
 * None of these three is a magic value -- each is the callee's own signed
 * 32-bit retval per alif_se_boot.h's shared retval contract (0 = SE-confirmed
 * success, <0 = LOCAL MHU transport failure, >0 = SE-REPORTED service
 * error), same reading rules as 0x0200000C above.
 */
#define BREADCRUMB_ADDR_EARLY        ((volatile uint32_t *)0x02000000u)
#define BREADCRUMB_ADDR_PRE_RELEASE  ((volatile uint32_t *)0x02000004u)
#define BREADCRUMB_ADDR_POST_RELEASE ((volatile uint32_t *)0x02000008u)
#define BREADCRUMB_ADDR_RETVAL       ((volatile uint32_t *)0x0200000Cu)
#define BREADCRUMB_ADDR_SE_TX_PID0   ((volatile uint32_t *)0x02000010u)
#define BREADCRUMB_ADDR_SE_TX_CID0   ((volatile uint32_t *)0x02000014u)
#define BREADCRUMB_ADDR_SE_TX_PID1   ((volatile uint32_t *)0x02000018u)
#define BREADCRUMB_ADDR_SE_TX_CID1   ((volatile uint32_t *)0x0200001Cu)
#define BREADCRUMB_ADDR_SE_RX_PID0   ((volatile uint32_t *)0x02000020u)
#define BREADCRUMB_ADDR_SE_RX_CID0   ((volatile uint32_t *)0x02000024u)
#define BREADCRUMB_ADDR_SE_RX_PID1   ((volatile uint32_t *)0x02000028u)
#define BREADCRUMB_ADDR_SE_RX_CID1   ((volatile uint32_t *)0x0200002Cu)
#define BREADCRUMB_ADDR_STEP_SET_VTOR   ((volatile uint32_t *)0x02000030u)
#define BREADCRUMB_ADDR_STEP_RESET_CPU  ((volatile uint32_t *)0x02000034u)
#define BREADCRUMB_ADDR_STEP_RELEASE_CPU ((volatile uint32_t *)0x02000038u)
/*
 * NEW word, added for CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY (../Kconfig) --
 * alif_se_process_toc_entry()'s own verbatim int return value, written
 * immediately after it returns. Only ever written when
 * CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY is on (see main()'s SECURE-ENCLAVE BOOT
 * step below) -- the SET_VTOR/RESET_CPU/RELEASE_CPU path above never touches
 * this word, and this path never touches 0x02000030-0x02000038. Picked as
 * the next free word AFTER the existing map's last word (0x02000038, this
 * app's per-step RELEASE_CPU breadcrumb) so it cannot collide with anything
 * above, and well below apps/dualcore_remote's beacon at
 * 0x02000100-0x02000114 (see that app's src/main.c) so it cannot collide with
 * that either.
 */
#define BREADCRUMB_ADDR_TOC_ENTRY_RETVAL ((volatile uint32_t *)0x02000040u)
/*
 * NEW word, added for CONFIG_DEMO_RELEASE_TOC_THEN_BOOT (../Kconfig) --
 * alif_se_boot_cpu()'s own verbatim int return value for that strategy's
 * SECOND step (the BOOT_CPU call following the un-defer), written
 * immediately after it returns. Only ever written when
 * CONFIG_DEMO_RELEASE_TOC_THEN_BOOT is on, and only if step 1
 * (alif_se_process_toc_entry(), written to 0x02000040 above) returned 0 --
 * that path's own #if block in main() stops at the first failure, same
 * contract as every other multi-step release sequence in this file.
 * 0x02000040 above is the SAME word CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY
 * writes -- both strategies call the identical PROCESS_TOC_ENTRY step 1, so
 * sharing that word is correct, not a collision. Picked 0x02000048,
 * deliberately SKIPPING 0x02000044: that word is unused by any strategy and
 * has been observed on the bench holding uninitialized SRAM garbage, which
 * has been useful as a written-vs-not control -- leaving it alone keeps
 * that control valid.
 */
#define BREADCRUMB_ADDR_TOC_THEN_BOOT_RETVAL ((volatile uint32_t *)0x02000048u)
#define BREADCRUMB_MAGIC_EARLY        0x5A5AA5A5u
#define BREADCRUMB_MAGIC_PRE_RELEASE  0xA5A55A5Au
#define BREADCRUMB_MAGIC_POST_RELEASE 0x5A5A5A5Au

/* ARM MHUv2 CoreSight identification-register offsets from a frame's base
 * (per the top-of-file "SE-MHU identity probe" section above) and the two
 * values every genuine ARM MHUv2 frame reads back at PID0/CID0. */
#define SE_MHU_ID_REG_PID0_OFFSET 0xFE0u
#define SE_MHU_ID_REG_PID1_OFFSET 0xFE4u
#define SE_MHU_ID_REG_CID0_OFFSET 0xFF0u
#define SE_MHU_ID_REG_CID1_OFFSET 0xFF4u
#define SE_MHU_ID_EXPECT_PID0 0x76u
#define SE_MHU_ID_EXPECT_CID0 0x0Du

static void breadcrumb_write(volatile uint32_t *addr, uint32_t magic)
{
	*addr = magic;
	sys_cache_data_flush_range((void *)addr, sizeof(*addr));
}

/*
 * SYS_INIT(..., EARLY, 0) is the earliest hook this app can reach without
 * hand-writing assembly. Per zephyr/kernel/init.c's z_cstart(), EARLY runs
 * before arch_kernel_init() (MPU setup), LOG_CORE_INIT(), device init, and
 * PRE_KERNEL_1 -- so this genuinely is the first C code belonging to THIS
 * APP to execute. What already ran before it on this SoC, verified against
 * this build rather than assumed: the reset handler
 * (arch/arm/core/cortex_m/reset.S), soc_reset_hook()
 * (soc/alif/ensemble/common/soc.c -- enables I/D cache;
 * CONFIG_SOC_RESET_HOOK is selected by soc/alif/ensemble/Kconfig), then
 * z_prep_c() (vector table relocation, FPU init, arch_bss_zero(),
 * arch_data_copy(), interrupt init) and z_cstart()'s own gcov hook. An
 * absent breadcrumb here does NOT mean "no instruction executed" -- it means
 * none of the above got far enough to reach this hook, which could still be
 * a few hundred instructions in.
 */
static int breadcrumb_mark_early(void)
{
	breadcrumb_write(BREADCRUMB_ADDR_EARLY, BREADCRUMB_MAGIC_EARLY);
	return 0;
}
SYS_INIT(breadcrumb_mark_early, EARLY, 0);

/*
 * SE-MHU identity probe -- see the top-of-file "SE-MHU identity probe"
 * section above for what this reads and why. Same disposable-thread +
 * bounded-wait shape as itcm_probe_thread_entry() below: an address with no
 * live peripheral behind it can bus-fault, and k_sys_fatal_error_handler()
 * (also below) aborts only the faulting thread, never main()'s.
 */
#define SE_MHU_ID_PROBE_TIMEOUT_MS  100
#define SE_MHU_ID_PROBE_STACK_SIZE  1024
#define SE_MHU_ID_PROBE_THREAD_PRIO 7
#define SE_MHU_ID_PROBE_WORD_COUNT  8

static K_SEM_DEFINE(se_mhu_id_probe_done_sem, 0, 1);
static K_THREAD_STACK_DEFINE(se_mhu_id_probe_stack, SE_MHU_ID_PROBE_STACK_SIZE);
static struct k_thread se_mhu_id_probe_thread;

/* Set true only by se_mhu_id_probe_thread_entry() running to completion (all
 * eight reads); read by main() only after se_mhu_id_probe_done_sem confirms
 * the thread finished -- a TIMED-OUT wait must not trust this flag, same
 * caveat as itcm_probe_ok below. Even when this stays false, some of
 * se_mhu_id_probe_words[] -- and the matching breadcrumb slots -- may still
 * hold real results captured before whichever read faulted.
 */
static volatile bool se_mhu_id_probe_ok;
/* Index order: 0=tx PID0, 1=tx CID0, 2=tx PID1, 3=tx CID1,
 *              4=rx PID0, 5=rx CID0, 6=rx PID1, 7=rx CID1. */
static uint32_t se_mhu_id_probe_words[SE_MHU_ID_PROBE_WORD_COUNT];

static void se_mhu_id_probe_thread_entry(void *p1, void *p2, void *p3)
{
	uint32_t tx_base;
	uint32_t rx_base;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* Same devicetree nodes modules/alif-se-boot/src/alif_se_boot.c
	 * reaches (by phandle) as se_boot_tx_base()/se_boot_rx_base() --
	 * reached here by nodelabel instead, since this file has no
	 * DT_DRV_COMPAT instance context, but it is the identical node either
	 * way. Never a second 0x40050000/0x40040000 literal.
	 */
	tx_base = DT_REG_ADDR(DT_NODELABEL(se_mhu_tx));
	rx_base = DT_REG_ADDR(DT_NODELABEL(se_mhu_rx));

	/* Each read is immediately breadcrumb-written (with its own cache
	 * flush) before the next one is attempted -- see the top-of-file
	 * comment for why: a fault on any one of these eight reads must not
	 * erase results already captured for the ones before it. */
	se_mhu_id_probe_words[0] = *(volatile uint32_t *)(tx_base + SE_MHU_ID_REG_PID0_OFFSET);
	breadcrumb_write(BREADCRUMB_ADDR_SE_TX_PID0, se_mhu_id_probe_words[0]);

	se_mhu_id_probe_words[1] = *(volatile uint32_t *)(tx_base + SE_MHU_ID_REG_CID0_OFFSET);
	breadcrumb_write(BREADCRUMB_ADDR_SE_TX_CID0, se_mhu_id_probe_words[1]);

	se_mhu_id_probe_words[2] = *(volatile uint32_t *)(tx_base + SE_MHU_ID_REG_PID1_OFFSET);
	breadcrumb_write(BREADCRUMB_ADDR_SE_TX_PID1, se_mhu_id_probe_words[2]);

	se_mhu_id_probe_words[3] = *(volatile uint32_t *)(tx_base + SE_MHU_ID_REG_CID1_OFFSET);
	breadcrumb_write(BREADCRUMB_ADDR_SE_TX_CID1, se_mhu_id_probe_words[3]);

	se_mhu_id_probe_words[4] = *(volatile uint32_t *)(rx_base + SE_MHU_ID_REG_PID0_OFFSET);
	breadcrumb_write(BREADCRUMB_ADDR_SE_RX_PID0, se_mhu_id_probe_words[4]);

	se_mhu_id_probe_words[5] = *(volatile uint32_t *)(rx_base + SE_MHU_ID_REG_CID0_OFFSET);
	breadcrumb_write(BREADCRUMB_ADDR_SE_RX_CID0, se_mhu_id_probe_words[5]);

	se_mhu_id_probe_words[6] = *(volatile uint32_t *)(rx_base + SE_MHU_ID_REG_PID1_OFFSET);
	breadcrumb_write(BREADCRUMB_ADDR_SE_RX_PID1, se_mhu_id_probe_words[6]);

	se_mhu_id_probe_words[7] = *(volatile uint32_t *)(rx_base + SE_MHU_ID_REG_CID1_OFFSET);
	breadcrumb_write(BREADCRUMB_ADDR_SE_RX_CID1, se_mhu_id_probe_words[7]);

	se_mhu_id_probe_ok = true;
	k_sem_give(&se_mhu_id_probe_done_sem);
}
#endif /* CONFIG_DEMO_EXECUTION_BREADCRUMB */

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

/*
 * ATOC entry name for the CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY path
 * (../Kconfig) -- the argument to alif_se_process_toc_entry(). A literal
 * here, not a Kconfig string, because there is exactly one such entry in
 * this repo's dual-core ATOC shape today (see the two-entry ATOC config
 * produced by the Alif SE tools' app-release-exec build config for this
 * demo's "ALP-HP" entry, cpu_id M55_HP, loadAddress 0x50000000 -- the same
 * peer this app's default CONFIG_DEMO_RELEASE_PEER_CPU_ID/
 * CONFIG_DEMO_RELEASE_PEER_ENTRY defaults already name) -- see ../Kconfig's
 * DEMO_RELEASE_VIA_TOC_ENTRY help text for why this is a #define instead.
 */
#define DEMO_RELEASE_TOC_ENTRY_ID "ALP-HP"

/*
 * --- What is now KNOWN about the DEFERRED ATOC path (bench-confirmed,
 * 2026-07-31) -- this file's own earlier guesses on this point have already
 * cost real bench time, so state the confirmed facts plainly instead of
 * re-deriving them from the SE-boot module's doc comments each time:
 *
 *   - "deferred" IS a valid entry in an ATOC entry's `flags` array (alongside
 *     "load"/"boot"/...) -- bench-confirmed, not the TBD alif_se_boot.h's
 *     alif_se_process_toc_entry() doc comment still describes. It sets
 *     TOC_IMAGE_DEFERRED (0x100) in that entry's on-the-wire flags word
 *     (observed going 0x00000022 -> 0x00000122) and shows as `D` in the SES
 *     table's flag column.
 *   - an entry flagged ["load","deferred"] is NOT loaded at cold boot -- the
 *     SES table shows a blank Dest Addr and Time 0.00 ms for it -- and IS
 *     loaded by the runtime alif_se_process_toc_entry() (service 500) call:
 *     a bench run watched SRAM at CONFIG_DEMO_RELEASE_PEER_ENTRY go from
 *     uninitialized to the staged binary's first 16 words, all matching,
 *     inside that one call.
 *   - service 500 returning 0 says PROCESS_TOC_ENTRY succeeded and the image
 *     is now resident -- it does NOT by itself mean the peer core started
 *     executing. CONFIG_DEMO_RELEASE_TOC_THEN_BOOT below exists because of
 *     exactly that: it follows the same un-defer call with the existing
 *     alif_se_boot_cpu() release call, in case the core still needs that
 *     explicit second step even after un-deferring places its image.
 */

/* Cadence of the PING loop. Not a hardware constant -- a demo-legibility
 * choice so a watcher reading two serial terminals can follow the exchange.
 */
#define PING_PERIOD_MS 500

/* How long to wait for the endpoint to bind before nagging the log. The
 * remote (dualcore_remote) may come up well after the host if the two images
 * are flashed/reset independently, so this is a "still waiting" heartbeat,
 * not a hard failure.
 */
#define BOUND_WAIT_S 5

/*
 * Wire format shared with apps/dualcore_remote/src/main.c -- both sides MUST
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

/* One completed PONG: the message itself plus the cycle count captured at
 * its arrival. Handed from ep_recv() (RPMsg backend's callback context) to
 * main() through pong_msgq below -- NOT through a pair of plain globals plus
 * a binary semaphore the way this used to work. A semaphore only serializes
 * the WAKEUP; it does nothing to stop a SECOND PONG's callback from
 * overwriting a shared global while main() is still in the middle of reading
 * the FIRST one out of it -- main() only takes the semaphore, it never holds
 * any lock the callback also respects, so that race is real, not
 * theoretical, on a link where the peer can echo faster than main() drains
 * pong_sem. k_msgq_put()/k_msgq_get() copy the whole struct pong_result by
 * value into/out of the queue's own ring buffer, so each side always has its
 * own private copy and a fast producer can never corrupt what a slower
 * consumer is still using.
 */
struct pong_result {
	struct ping_pong_msg msg;
	uint32_t             cycles;
};

/* Capacity 1: this demo only ever cares about the MOST RECENT PONG, matching
 * the previous binary-semaphore handoff's semantics -- see ep_recv() below
 * for how a still-full queue (main() fell behind) is purged before the
 * newest result is queued, rather than the newest result being silently
 * dropped. Alignment 4 matches struct pong_result's own natural alignment
 * (its widest member is a uint32_t).
 */
K_MSGQ_DEFINE(pong_msgq, sizeof(struct pong_result), 1, 4);

static void ep_bound(void *priv)
{
	ARG_UNUSED(priv);
	k_sem_give(&bound_sem);
}

static void ep_recv(const void *data, size_t len, void *priv)
{
	struct pong_result result;

	ARG_UNUSED(priv);

	if (len != sizeof(result.msg)) {
		LOG_ERR("PONG has unexpected length %zu (expected %zu); dropping", len,
			sizeof(result.msg));
		return;
	}

	/* Timestamp taken AFTER the length check, not before: capturing it
	 * first would let a malformed message clobber the round-trip
	 * timestamp of the NEXT valid PONG, since this function returns here
	 * without ever queuing an entry for a message that fails the check --
	 * an early timestamp write is not undone just because the rest of the
	 * message was garbage.
	 */
	result.cycles = k_cycle_get_32();
	memcpy(&result.msg, data, sizeof(result.msg));

	/* K_NO_WAIT: this callback runs on the RPMsg backend's own context
	 * and must never block. If the queue is still full (main() has not
	 * drained the previous PONG yet), purge it first so this newer result
	 * always wins instead of being silently dropped by a failing
	 * k_msgq_put() on a full queue -- k_msgq_purge() cannot fail on a
	 * queue with no waiting receivers, so the retry below is guaranteed
	 * to succeed.
	 */
	if (k_msgq_put(&pong_msgq, &result, K_NO_WAIT) != 0) {
		k_msgq_purge(&pong_msgq);
		(void)k_msgq_put(&pong_msgq, &result, K_NO_WAIT);
	}
}

static struct ipc_ept_cfg ep_cfg = {
    .name = "dualcore_ping_pong",
    .cb =
        {
            .bound = ep_bound,
            .received = ep_recv,
        },
};

#ifdef CONFIG_DEMO_EXECUTION_BREADCRUMB
/*
 * --- Pre-release ITCM-placement probe (bench diagnostic, NOT shipped) ------
 *
 * Off by default (CONFIG_DEMO_EXECUTION_BREADCRUMB, see ../Kconfig) --
 * enabled only with `-DEXTRA_CONF_FILE=breadcrumb.conf` (../breadcrumb.conf),
 * the same opt-in shape every other bench-diagnostic probe in this file
 * uses. This probe deliberately reads a possibly-unmapped address
 * (CONFIG_DEMO_RELEASE_PEER_ENTRY, before the peer core is released), so it
 * has no business running in the shipped default image -- the shipped image
 * carries none of this, same as the execution breadcrumbs and the SE-MHU
 * identity probe above.
 *
 * alif_se_boot_cpu()'s own doc (modules/alif-se-boot/include/alif_se_boot.h)
 * says the call does NOT place any code at the peer's entry address -- the
 * image must already be resident there, normally because the Secure Enclave
 * Services (SES) placed it while processing a two-entry ATOC whose entry for
 * that core is flagged ["load"]. This app has never independently checked
 * that claim. If the peer never comes up, "SES never placed the image" and
 * "SES placed it fine but the release/boot step itself is broken" look
 * identical from this app's own PING/PONG timeout -- and the bench could not
 * tell them apart either: the peer's ITCM was unreadable from both SWD
 * access ports pre-release, which is a DEBUG-port limitation, not
 * necessarily the same path a CPU-side load takes over the system bus.
 * Reading CONFIG_DEMO_RELEASE_PEER_ENTRY here, immediately before the
 * release call, is the one probe left that can still separate the two
 * mechanisms with no debugger involved: if the first words already look
 * like a vector table, SES placement worked and any later failure is at or
 * after release; if the read comes back garbage -- or faults outright --
 * SES never placed anything there.
 *
 * The read targets the peer's ITCM over the global bus alias, from THIS
 * core, before the peer is released -- that memory may not be powered,
 * clocked, or bus-reachable yet, so unlike this file's other
 * alif_se_boot_cpu() failure handling, an unreadable address here can raise
 * a CPU exception (BusFault/SecureFault), not just return an error code. To
 * keep that from taking the whole demo down, the read runs in its own
 * disposable thread (itcm_probe_thread_entry()), and
 * k_sys_fatal_error_handler() below is overridden so a fault in that thread
 * aborts only that thread -- see its comment for why that is the genuinely
 * safe outcome here, not just a masked crash. main() then waits on
 * itcm_probe_done_sem with a bounded timeout rather than K_FOREVER, so it
 * proceeds either way: clean read, faulted read, or (in the theoretical case
 * the fault handler below never gets the chance to run at all) no result
 * within the bound.
 */
#define ITCM_PROBE_WORD_COUNT  4
#define ITCM_PROBE_TIMEOUT_MS  100
#define ITCM_PROBE_STACK_SIZE  1024
#define ITCM_PROBE_THREAD_PRIO 7

static K_SEM_DEFINE(itcm_probe_done_sem, 0, 1);
static K_THREAD_STACK_DEFINE(itcm_probe_stack, ITCM_PROBE_STACK_SIZE);
static struct k_thread itcm_probe_thread;

/* Set true only by itcm_probe_thread_entry() running to completion; read by
 * main() only after itcm_probe_done_sem confirms the thread finished -- a
 * TIMED-OUT wait must not trust this flag, since the thread that would set
 * it may have been aborted mid-read by a fault.
 */
static volatile bool itcm_probe_ok;
static uint32_t       itcm_probe_words[ITCM_PROBE_WORD_COUNT];

/*
 * Overrides Zephyr's weak k_sys_fatal_error_handler() (zephyr/kernel/fatal.c)
 * for THIS WHOLE IMAGE -- there is only one such hook per image, it is not
 * scoped to the probe below -- but ONLY when CONFIG_DEMO_EXECUTION_BREADCRUMB
 * is on: this whole function is compiled inside that option's #ifdef, same as
 * the two probes it exists for. A build WITHOUT this option gets Zephyr's
 * normal default behaviour (arch_system_halt() on any fatal error) --
 * downgrading every fatal fault image-wide to "log and abort the faulting
 * thread" is a bench-diagnostic accommodation for these two deliberately
 * fault-prone probes, not something the shipped default image should carry.
 * The default implementation calls arch_system_halt() and never returns,
 * which would spin this core forever on a probe fault -- indistinguishable
 * from the SE-boot hang these probes exist to rule out. Per
 * k_sys_fatal_error_handler()'s own documented contract
 * (zephyr/include/zephyr/fatal.h): "If this function returns, then the
 * currently executing thread will be aborted" -- i.e. z_fatal_error() falls
 * through to k_thread_abort() on whichever thread faulted, instead of halting
 * the system. Because each probe read runs in its own disposable thread
 * (itcm_probe_thread / se_mhu_id_probe_thread) rather than main()'s thread, a
 * fault there tears down only that thread; main() is unaffected and finds out
 * via that probe's own bounded semaphore wait, not via this handler.
 *
 * Because a fault ANYWHERE in this image (not just in the two probe threads)
 * now gets this same "log and abort the faulting thread" policy while this
 * option is on, the log line below reports which thread actually faulted --
 * via k_current_get()/k_thread_name_get() -- rather than assuming it was one
 * of the two bench probes. main() itself can fault too (e.g. in the RPMsg
 * path below), and asserting "a bench-diagnostic probe thread ... main()
 * continues" in that case would be actively misleading.
 *
 * This same override covers se_mhu_id_probe_thread_entry() (see the
 * top-of-file "SE-MHU identity probe" section) as well as
 * itcm_probe_thread_entry() below -- there is still only one
 * k_sys_fatal_error_handler() per image, and both bench-diagnostic probes
 * share it deliberately: whichever of the two disposable threads faults,
 * only that thread is aborted, and any breadcrumb slot the faulting probe
 * had not yet reached simply stays absent.
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	k_tid_t     faulting_thread = k_current_get();
	const char *thread_name     = k_thread_name_get(faulting_thread);

	ARG_UNUSED(esf);
	LOG_ERR("CPU exception (reason=%u) in thread %p (\"%s\") -- only this thread is "
	        "aborted; every other thread, including main() if it is not the one that "
	        "faulted, continues",
	        reason, (void *)faulting_thread, thread_name != NULL ? thread_name : "<unnamed>");
}

static void itcm_probe_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	volatile uint32_t *peer_itcm = (volatile uint32_t *)CONFIG_DEMO_RELEASE_PEER_ENTRY;

	/* Any of these four reads may fault -- see k_sys_fatal_error_handler()
	 * above for what happens then.
	 */
	for (int i = 0; i < ITCM_PROBE_WORD_COUNT; i++) {
		itcm_probe_words[i] = peer_itcm[i];
	}

	itcm_probe_ok = true;
	k_sem_give(&itcm_probe_done_sem);
}
#endif /* CONFIG_DEMO_EXECUTION_BREADCRUMB */

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

#ifdef CONFIG_DEMO_EXECUTION_BREADCRUMB
	/*
	 * SE-MHU identity probe -- see the top-of-file "SE-MHU identity probe"
	 * section for why this exists. Runs first, BEFORE the pre-release
	 * breadcrumb (0x02000004) and BEFORE the pre-release ITCM probe below,
	 * so the MHUv2 identity readout is independent of whether either of
	 * those later steps is ever reached. Same disposable-thread +
	 * bounded-wait shape as the ITCM probe.
	 */
	se_mhu_id_probe_ok = false;
	k_thread_create(&se_mhu_id_probe_thread, se_mhu_id_probe_stack,
			K_THREAD_STACK_SIZEOF(se_mhu_id_probe_stack), se_mhu_id_probe_thread_entry,
			NULL, NULL, NULL, K_PRIO_PREEMPT(SE_MHU_ID_PROBE_THREAD_PRIO), 0, K_NO_WAIT);

	if (k_sem_take(&se_mhu_id_probe_done_sem, K_MSEC(SE_MHU_ID_PROBE_TIMEOUT_MS)) != 0) {
		LOG_WRN("SE-MHU identity probe: no result within %d ms",
			SE_MHU_ID_PROBE_TIMEOUT_MS);
	} else if (se_mhu_id_probe_ok) {
		bool tx_ok = (se_mhu_id_probe_words[0] == SE_MHU_ID_EXPECT_PID0) &&
			     (se_mhu_id_probe_words[1] == SE_MHU_ID_EXPECT_CID0);
		bool rx_ok = (se_mhu_id_probe_words[4] == SE_MHU_ID_EXPECT_PID0) &&
			     (se_mhu_id_probe_words[5] == SE_MHU_ID_EXPECT_CID0);

		LOG_INF("SE-MHU identity probe: tx(0x%08x) PID0=0x%02x CID0=0x%02x -- %s",
			DT_REG_ADDR(DT_NODELABEL(se_mhu_tx)), se_mhu_id_probe_words[0],
			se_mhu_id_probe_words[1],
			tx_ok ? "MHUv2 identity confirmed" : "NOT an MHUv2 at this address");
		LOG_INF("SE-MHU identity probe: rx(0x%08x) PID0=0x%02x CID0=0x%02x -- %s "
		        "(ARM MHUv2 expects PID0=0x%02x CID0=0x%02x)",
			DT_REG_ADDR(DT_NODELABEL(se_mhu_rx)), se_mhu_id_probe_words[4],
			se_mhu_id_probe_words[5],
			rx_ok ? "MHUv2 identity confirmed" : "NOT an MHUv2 at this address",
			SE_MHU_ID_EXPECT_PID0, SE_MHU_ID_EXPECT_CID0);
	}
	/* else: se_mhu_id_probe_ok stayed false because a read faulted --
	 * k_sys_fatal_error_handler() below already logged that outcome; any
	 * registers read before the fault are still in the breadcrumb slots
	 * (0x02000010-0x0200002C, see the top-of-file comment).
	 */
#endif /* CONFIG_DEMO_EXECUTION_BREADCRUMB */

	/*
	 * Release the peer core via the Secure Enclave BEFORE opening ipc0.
	 * Do NOT abort on failure: log every distinct outcome class (local
	 * transport failure vs. SE-reported service error vs. success) and
	 * continue into the RPMsg host path regardless, so the demo still
	 * shows this side running even when the peer never comes up -- and
	 * says out loud why.
	 *
	 * alif_se_start_cpu() (modules/alif-se-boot) -- SET_VTOR, then
	 * RESET_CPU, then RELEASE_CPU -- replaces the alif_se_boot_cpu()-only
	 * call an earlier revision of this file used: that call never
	 * transferred a vector table base into the released core's own VTOR
	 * register, which is what a bench run traced the peer's
	 * CFSR==IACCVIOL/HFSR==FORCED lockup to. See this file's top-of-file
	 * comment and include/alif_se_boot.h's alif_se_start_cpu() doc comment
	 * for the Alif DFP documentation this ordering rests on.
	 * UNVERIFIED ON SILICON in this exact three-call form -- see
	 * modules/alif-se-boot/README.md. CONFIG_DEMO_RELEASE_PEER_CPU_ID /
	 * CONFIG_DEMO_RELEASE_PEER_ENTRY (../Kconfig) name the peer and the
	 * entry address reported to the SE.
	 *
	 * When CONFIG_DEMO_EXECUTION_BREADCRUMB is on, the call below is
	 * decomposed into its three underlying steps (same functions, same
	 * order alif_se_start_cpu() itself uses) so each step's completion can
	 * be breadcrumbed individually -- see the top-of-file "per-step SE
	 * release-sequence breadcrumbs" section. Otherwise it is the single
	 * alif_se_start_cpu() call.
	 */

#ifdef CONFIG_DEMO_EXECUTION_BREADCRUMB
	/*
	 * Pre-release SES-placement probe -- see the top-of-file comment above
	 * itcm_probe_thread_entry() for why this exists and why it runs in its
	 * own thread. Fire it, wait for a result with a bound (so a fault or a
	 * genuinely stuck read can never hang this thread), then log whichever
	 * outcome happened before moving on to the release call itself. Gated
	 * behind CONFIG_DEMO_EXECUTION_BREADCRUMB -- see that section's own
	 * top-of-file comment -- because it deliberately reads a
	 * possibly-unmapped address before the peer core is released, which is
	 * bench scaffolding, not something the shipped default image should do.
	 */
	itcm_probe_ok = false;
	k_thread_create(&itcm_probe_thread, itcm_probe_stack, K_THREAD_STACK_SIZEOF(itcm_probe_stack),
			itcm_probe_thread_entry, NULL, NULL, NULL,
			K_PRIO_PREEMPT(ITCM_PROBE_THREAD_PRIO), 0, K_NO_WAIT);

	if (k_sem_take(&itcm_probe_done_sem, K_MSEC(ITCM_PROBE_TIMEOUT_MS)) != 0) {
		LOG_WRN("pre-release ITCM probe: no result within %d ms -- treating 0x%08x "
		        "as unreadable pre-release",
		        ITCM_PROBE_TIMEOUT_MS, CONFIG_DEMO_RELEASE_PEER_ENTRY);
	} else if (itcm_probe_ok) {
		LOG_INF("pre-release ITCM probe: 0x%08x = %08x %08x %08x %08x -- compare "
		        "against the REMOTE image's own first four words (its "
		        "zephyr.elf vector table start; changes every rebuild, not "
		        "available to this build -- see the top-of-file comment above "
		        "itcm_probe_thread_entry())",
		        CONFIG_DEMO_RELEASE_PEER_ENTRY, itcm_probe_words[0], itcm_probe_words[1],
		        itcm_probe_words[2], itcm_probe_words[3]);
	}
	/* else: itcm_probe_ok stayed false because the read faulted --
	 * k_sys_fatal_error_handler() above already logged that outcome.
	 */
#endif /* CONFIG_DEMO_EXECUTION_BREADCRUMB */

#ifdef CONFIG_DEMO_EXECUTION_BREADCRUMB
	/* Second breadcrumb -- see the top-of-file comment above
	 * breadcrumb_mark_early() for what these words together mean.
	 */
	breadcrumb_write(BREADCRUMB_ADDR_PRE_RELEASE, BREADCRUMB_MAGIC_PRE_RELEASE);
#endif

#if defined(CONFIG_DEMO_RELEASE_TOC_THEN_BOOT)
	/*
	 * Alternate release strategy #2 (../Kconfig: CONFIG_DEMO_RELEASE_TOC_THEN_BOOT,
	 * default OFF). Tests the remaining candidate after
	 * CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY alone: that path's un-defer call
	 * bench-confirmed it MATERIALIZES the peer's image (see the "known"
	 * comment block above DEMO_RELEASE_TOC_ENTRY_ID), but the peer still did
	 * not start executing. This does BOTH, in order, stopping at the first
	 * failure -- step 1 is the identical alif_se_process_toc_entry() call
	 * CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY makes; step 2 is this file's
	 * original alif_se_boot_cpu() release call (service_id 501, BOOT_CPU)
	 * against the SAME cpu_id/entry every other branch here uses. `ret` ends
	 * up holding whichever step actually ran last -- step 1's retval if step
	 * 1 failed, otherwise step 2's -- so the LOG_* block below this whole
	 * #if/#elif/#else/#endif reads `ret` identically to every other branch.
	 */
	ret = alif_se_process_toc_entry(DEMO_RELEASE_TOC_ENTRY_ID);
#ifdef CONFIG_DEMO_EXECUTION_BREADCRUMB
	breadcrumb_write(BREADCRUMB_ADDR_TOC_ENTRY_RETVAL, (uint32_t)ret);
#endif
	if (ret == 0) {
		ret = alif_se_boot_cpu(CONFIG_DEMO_RELEASE_PEER_CPU_ID, CONFIG_DEMO_RELEASE_PEER_ENTRY);
#ifdef CONFIG_DEMO_EXECUTION_BREADCRUMB
		breadcrumb_write(BREADCRUMB_ADDR_TOC_THEN_BOOT_RETVAL, (uint32_t)ret);
#endif
	}
#elif defined(CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY)
	/*
	 * Alternate release strategy (../Kconfig: CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY,
	 * default OFF -- selecting this branch is a deliberate build-time
	 * choice, never a fallback). Calls SE service_id 500 (PROCESS_TOC_ENTRY)
	 * against the ATOC entry named DEMO_RELEASE_TOC_ENTRY_ID instead of the
	 * BOOT_CPU/SET_VTOR/RESET_CPU/RELEASE_CPU surface every other branch
	 * here uses -- see alif_se_process_toc_entry()'s doc comment
	 * (modules/alif-se-boot/include/alif_se_boot.h) for the full account,
	 * including the DEFERRED-flag ATOC requirement this path rests on.
	 * `ret` ends up holding this call's own retval, read by the same
	 * 0/`<0`/`>0` contract as every other branch, so the LOG_* block below
	 * this whole #if/#elif/#else/#endif reads `ret` identically regardless
	 * of which branch ran.
	 */
	ret = alif_se_process_toc_entry(DEMO_RELEASE_TOC_ENTRY_ID);
#ifdef CONFIG_DEMO_EXECUTION_BREADCRUMB
	breadcrumb_write(BREADCRUMB_ADDR_TOC_ENTRY_RETVAL, (uint32_t)ret);
#endif
#elif defined(CONFIG_DEMO_EXECUTION_BREADCRUMB)
	/*
	 * Decomposed form: the same three calls alif_se_start_cpu() makes,
	 * in the same order, stopping at the first failure -- see the
	 * top-of-file "per-step SE release-sequence breadcrumbs" section for
	 * why this build breaks the sequence open instead of calling
	 * alif_se_start_cpu() as one opaque call. `ret` ends up holding
	 * exactly what alif_se_start_cpu() itself would have returned for the
	 * same three outcomes, so everything below this #if/#elif/#else/#endif
	 * reads `ret` identically either way.
	 */
	ret = alif_se_set_vtor(CONFIG_DEMO_RELEASE_PEER_CPU_ID, CONFIG_DEMO_RELEASE_PEER_ENTRY);
	breadcrumb_write(BREADCRUMB_ADDR_STEP_SET_VTOR, (uint32_t)ret);
	if (ret == 0) {
		ret = alif_se_reset_cpu(CONFIG_DEMO_RELEASE_PEER_CPU_ID);
		breadcrumb_write(BREADCRUMB_ADDR_STEP_RESET_CPU, (uint32_t)ret);
	}
	if (ret == 0) {
		ret = alif_se_release_cpu(CONFIG_DEMO_RELEASE_PEER_CPU_ID);
		breadcrumb_write(BREADCRUMB_ADDR_STEP_RELEASE_CPU, (uint32_t)ret);
	}
#else
	ret = alif_se_start_cpu(CONFIG_DEMO_RELEASE_PEER_CPU_ID, CONFIG_DEMO_RELEASE_PEER_ENTRY);
#endif

#ifdef CONFIG_DEMO_EXECUTION_BREADCRUMB
	/* Third and fourth breadcrumbs -- MUST be the first thing that runs
	 * after the release sequence returns, before the LOG_* branches below
	 * or anything else. Presence of 0x02000008 alone answers "did the
	 * sequence return at all"; 0x0200000C carries the verbatim overall
	 * return value for everything downstream of that. See the top-of-file
	 * comment above breadcrumb_mark_early() for the full decision tree.
	 */
	breadcrumb_write(BREADCRUMB_ADDR_POST_RELEASE, BREADCRUMB_MAGIC_POST_RELEASE);
	breadcrumb_write(BREADCRUMB_ADDR_RETVAL, (uint32_t)ret);
#endif

#if defined(CONFIG_DEMO_RELEASE_TOC_THEN_BOOT)
	if (ret == 0) {
		LOG_INF("SE PROCESS_TOC_ENTRY(image_id=%s) then BOOT_CPU(cpu_id=%u, entry=0x%08x) "
		        "both succeeded",
		        DEMO_RELEASE_TOC_ENTRY_ID, CONFIG_DEMO_RELEASE_PEER_CPU_ID,
		        CONFIG_DEMO_RELEASE_PEER_ENTRY);
	} else if (ret < 0) {
		LOG_ERR("CONFIG_DEMO_RELEASE_TOC_THEN_BOOT local MHU transport failure (%d) -- with "
		        "CONFIG_DEMO_EXECUTION_BREADCRUMB on, see 0x02000040 (PROCESS_TOC_ENTRY "
		        "retval) vs. 0x02000048 (BOOT_CPU retval, present only if step 1 succeeded) "
		        "for which step this was; continuing as RPMsg host anyway, the peer will "
		        "never bind",
		        ret);
	} else {
		LOG_ERR("CONFIG_DEMO_RELEASE_TOC_THEN_BOOT SE-reported service error (%d) -- with "
		        "CONFIG_DEMO_EXECUTION_BREADCRUMB on, see 0x02000040 (PROCESS_TOC_ENTRY "
		        "retval) vs. 0x02000048 (BOOT_CPU retval, present only if step 1 succeeded) "
		        "for which step this was; continuing as RPMsg host anyway, the peer will "
		        "never bind",
		        ret);
	}
#elif defined(CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY)
	if (ret == 0) {
		LOG_INF("SE PROCESS_TOC_ENTRY(image_id=%s) succeeded", DEMO_RELEASE_TOC_ENTRY_ID);
	} else if (ret < 0) {
		LOG_ERR("alif_se_process_toc_entry(image_id=%s) local MHU transport failure (%d) "
		        "-- peer was NOT released via this path; continuing as RPMsg host anyway, "
		        "the peer will never bind",
		        DEMO_RELEASE_TOC_ENTRY_ID, ret);
	} else {
		LOG_ERR("alif_se_process_toc_entry(image_id=%s) SE-reported service error (%d) -- "
		        "peer was NOT released via this path; continuing as RPMsg host anyway, the "
		        "peer will never bind",
		        DEMO_RELEASE_TOC_ENTRY_ID, ret);
	}
#else
	if (ret == 0) {
		LOG_INF("SE released peer cpu_id=%u, reported entry 0x%08x",
			CONFIG_DEMO_RELEASE_PEER_CPU_ID, CONFIG_DEMO_RELEASE_PEER_ENTRY);
	} else if (ret < 0) {
		LOG_ERR("alif_se_start_cpu() local MHU transport failure (%d) -- peer cpu_id=%u "
		        "was NOT released; continuing as RPMsg host anyway, the peer will never "
		        "bind",
		        ret, CONFIG_DEMO_RELEASE_PEER_CPU_ID);
	} else {
		LOG_ERR("alif_se_start_cpu() SE-reported service error (%d) -- peer cpu_id=%u was "
		        "NOT released; continuing as RPMsg host anyway, the peer will never bind",
		        ret, CONFIG_DEMO_RELEASE_PEER_CPU_ID);
	}
#endif

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
		        "remote to register its endpoint",
		        BOUND_WAIT_S);
	}
	LOG_INF("endpoint bound; starting PING/PONG");

	for (;;) {
		struct ping_pong_msg ping = { .seq = seq };
		struct pong_result   pong;
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
		 * operation: the remote logs and discards the echo when its own
		 * ipc_service_send() fails (see apps/dualcore_remote/src/main.c). With
		 * K_FOREVER a single such drop parks this thread permanently -- the demo
		 * goes silent with no diagnostic, which is the worst failure mode to hit
		 * in front of a customer. Two PING periods is generous: a doorbell round
		 * trip between two M55s on the same die is microseconds, not
		 * milliseconds.
		 */
		if (k_msgq_get(&pong_msgq, &pong, K_MSEC(PING_PERIOD_MS * 2)) != 0) {
			LOG_ERR("no PONG for seq=%u within %d ms -- dropping it and "
			        "continuing with the next sequence number",
			        seq, PING_PERIOD_MS * 2);
			seq++;
			k_msleep(PING_PERIOD_MS);
			continue;
		}

		if (pong.msg.seq != seq) {
			LOG_WRN("PONG seq mismatch: sent %u, got %u", seq, pong.msg.seq);
		}

		rtt_us = k_cyc_to_us_floor32((uint32_t)(pong.cycles - send_cycles));
		LOG_INF("PONG seq=%u rtt=%u us", pong.msg.seq, rtt_us);

		seq++;
		k_msleep(PING_PERIOD_MS);
	}
}
