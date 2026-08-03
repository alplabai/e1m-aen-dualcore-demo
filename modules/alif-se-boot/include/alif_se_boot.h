/*
 * Copyright 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public API for the Alp Lab Alif Secure-Enclave BOOT_CPU service client
 * (modules/alif-se-boot). See README.md for the module-level writeup,
 * including the "authored from the protocol, not from Alif's source" note
 * and the "UNVERIFIED ON SILICON" caveat -- both apply to everything behind
 * this header.
 */

#ifndef ALP_LAB_ALIF_SE_BOOT_H_
#define ALP_LAB_ALIF_SE_BOOT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SE-domain CPU ids for service_id 501 (BOOT_CPU)'s `send_cpu_id` field, as
 * transcribed in the calling task's protocol brief. Only M55_HP is exercised
 * by this repo's bench-proven shape (apps/dualcore_host releasing
 * apps/dualcore_remote; on the E1M-AEN801 bench that is RTSS-HE releasing
 * RTSS-HP -- see docs/BENCH-DUALCORE.md section 0 and
 * apps/dualcore_host/Kconfig's CONFIG_DEMO_RELEASE_PEER_CPU_ID); the other
 * three are listed here only so a caller never has to invent or guess one.
 */
#define ALIF_SE_BOOT_CPU_A32_0 0U
#define ALIF_SE_BOOT_CPU_A32_1 1U
#define ALIF_SE_BOOT_CPU_M55_HP 2U
#define ALIF_SE_BOOT_CPU_M55_HE 3U

/**
 * @brief Ask the Alif Secure Enclave to un-defer and process an ATOC (Application
 *        TOC) entry by name.
 *
 * Issues Secure-Enclave service_id 500 (PROCESS_TOC_ENTRY) over the same
 * SE-service MHUv2 transport as every other function in this header, with the
 * same readiness-heartbeat gate. Uses a DIFFERENT, 20-byte wire struct from
 * every other call in this file -- confirmed by Alif's DFP se_services
 * headers as `process_toc_entry_svc_t`: the same 8-byte header, plus an
 * 8-byte `send_entry_id` (the TOC entry's `image_identifier`, an ASCII NAME,
 * not a `send_cpu_id`), plus a 4-byte `resp_error_code`. It does NOT take a
 * cpu_id or an address at all -- which CPU (if any) ends up released is
 * entirely a property of the NAMED TOC ENTRY's own flags, decided by the SE
 * when it processes that entry, not by this call's arguments.
 *
 * REQUIRES THE NAMED ENTRY TO ALREADY BE IN A DEFERRED STATE. Per Alif's DFP
 * (`services_host_boot.c`'s doc comment on
 * `SERVICES_boot_process_toc_entry()`): "The TOC entry should also be in a
 * DEFERRED state which means on Boot up it is not automatically booted by
 * SES. This SERVICE call will un-defer the TOC entry" -- i.e. this is the
 * SES-driven counterpart to alif_se_boot_cpu()'s manual BOOT_CPU path: instead
 * of this core telling the SE "release cpu_id at entry_addr" against an entry
 * that SES already auto-loaded-but-not-booted at cold boot (a plain `["load"]`
 * ATOC flag, which the SES flag table reports as `uLV` -- Loaded + Verified,
 * NOT Booted -- see the ATOC note below), this call instead tells the SE "now
 * go ahead and act on entry @p image_id", with SES itself doing whatever
 * placement/verification/boot that entry's own flags call for AT THIS CALL,
 * not at cold boot. This is the mechanism this repo's `["load"]`-only ATOC
 * shape does NOT exercise -- see CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY in
 * apps/dualcore_host/Kconfig.
 *
 * WHAT THE ATOC MUST LOOK LIKE FOR THIS PATH -- BENCH-CONFIRMED 2026-07-31:
 * `"deferred"` is a valid MEMBER of an ATOC entry's `flags` ARRAY, alongside
 * `"load"` / `"boot"` (e.g. `["load", "boot", "deferred"]`) -- NOT a sibling
 * `"deferred": true` KEY on the entry, which the ATOC builder rejects. It sets
 * `TOC_IMAGE_DEFERRED = 0x100` (per Alif's DFP source,
 * `se_services/templates/services_test.c`) in that entry's on-the-wire flags
 * word -- observed on the bench going `0x00000022` -> `0x00000122` when
 * `"deferred"` was added to a working entry's flag list. The SES boot table
 * reports this with the letter **`D`** in its flag column, at flag-string
 * legend position `FLAG_STRING_DEFERRED` (index 5).
 *
 * A working peer entry is `["load", "boot", "deferred"]`. With `"deferred"`
 * set, the SES ignores `"boot"` at cold boot: the table shows `uLs  D` in the
 * flag column, a blank Dest Addr, and Time `0.00 ms` for that entry -- i.e.
 * the SE loads and verifies the image but does not run it. Calling
 * `alif_se_process_toc_entry()` afterwards issues `SERVICES_boot_process_toc_entry`
 * (service 500), which performs load, verify, AND release together for that
 * entry.
 *
 * The sample ATOC configs used to confirm this live under Alif SETOOLS'
 * `app-release-exec-linux/build/config` directory (including this repo's own
 * two-entry shapes, e.g. `aen-dc-hostB-two-entry.json`).
 *
 * REMAINING CAVEAT: a 0 return from this function means service 500
 * (PROCESS_TOC_ENTRY) succeeded and the image is resident -- by itself it
 * does NOT prove the peer core began executing. See
 * CONFIG_DEMO_RELEASE_TOC_THEN_BOOT (apps/dualcore_host/Kconfig) for the
 * belt-and-suspenders follow-up call this repo's demo uses to close that gap.
 *
 * BENCH-CONFIRMED for the ATOC-flag mechanics above (2026-07-31). The
 * question of whether the PEER CORE itself began executing after this call is
 * a separate, still-open question -- see the caveat immediately above.
 *
 * @param image_id ASCII name of the TOC entry to process, e.g. "ALP-HP" --
 *                 matches that entry's `image_identifier` field in the ATOC.
 *                 Copied into the wire struct's 8-byte fixed-width field with
 *                 the same `strncpy()` semantics Alif's own wrapper uses: at
 *                 most 8 bytes are sent, zero-padded if @p image_id is
 *                 shorter, and NOT NUL-terminated on the wire if @p image_id
 *                 is 8 bytes or longer (this is a raw TOC field, not a C
 *                 string). @p image_id itself must be a NUL-terminated C
 *                 string (this function reads it with `strncpy()`).
 *
 * @retval 0        The SE reports PROCESS_TOC_ENTRY succeeded (same
 *                  both-fields-zero rule as alif_se_boot_cpu()'s 0 retval).
 * @retval -EINVAL  @p image_id is NULL, or its length exceeds
 *                  ALIF_SE_TOC_ENTRY_ID_LEN (8) bytes -- checked before
 *                  anything is sent to the SE, so a name that would otherwise
 *                  be silently truncated by the wire copy (and end up
 *                  addressing a DIFFERENT TOC entry) is rejected instead.
 * @retval <0       Any other LOCAL transport failure, or -ENOTCONN if the SE
 *                  never answered the readiness heartbeat -- see
 *                  alif_se_boot_cpu()'s retval doc above for the full
 *                  breakdown.
 * @retval >0       An SE-REPORTED error, clamped to INT_MAX -- see
 *                  alif_se_boot_cpu()'s retval doc above.
 */
int alif_se_process_toc_entry(const char *image_id);

/**
 * @brief Ask the Alif Secure Enclave to release (boot) an RTSS/A32 core.
 *
 * Issues Secure-Enclave service_id 501 (BOOT_CPU) over the dedicated
 * SE-service MHUv2 frame pair named by this module's devicetree node
 * (compatible "alplab,e8-se-boot"), using the small SRAM carve-out named by
 * that node's `memory-region` property to hold the request/response
 * structure.
 *
 * This call does NOT place any code at @p entry_addr. The target core's
 * image must already be resident there -- normally because the Secure
 * Enclave Services (SES) loaded it there while processing a two-entry ATOC
 * (Application TOC) whose entry for that core is flagged `["load"]`. Calling
 * this function against an entry address with no image present will start
 * the target core executing garbage.
 *
 * This call ALSO does NOT transfer any vector table base into the target
 * core's own VTOR register, even if @p entry_addr looks like one. Per Alif's
 * documentation of `SERVICES_boot_set_vtor()` and `SERVICES_boot_reset_cpu()`:
 * SET_VTOR (service 505, alif_se_set_vtor()) only ever writes a GLOBAL
 * SE-side VTOR register, and only a subsequent RESET_CPU (service 503,
 * alif_se_reset_cpu()) transfers that global value into the core's actual
 * internal VTOR -- see alif_se_reset_cpu()'s doc comment below for the scope
 * of what has and has not been observed of that transfer on real silicon.
 * Calling this function on its own, without that SET_VTOR/RESET_CPU pair
 * having already run, releases the core with whatever internal VTOR it
 * already had -- 0x00000000 on a cold release -- so it fetches its initial
 * SP/PC from its own local address 0, not from @p entry_addr. This function
 * alone is therefore NOT sufficient to start a core that needs a real
 * vector table base; use alif_se_start_cpu() for the sequence that gets
 * that right.
 *
 * UNVERIFIED ON SILICON -- see README.md.
 *
 * @param cpu_id     SE-domain CPU id: one of ALIF_SE_BOOT_CPU_A32_0,
 *                   ALIF_SE_BOOT_CPU_A32_1, ALIF_SE_BOOT_CPU_M55_HP,
 *                   ALIF_SE_BOOT_CPU_M55_HE.
 * @param entry_addr Entry address the target core should start executing
 *                   from (e.g. 0x58000000, the RTSS-HE ITCM global alias
 *                   also used as the ATOC `loadAddress` for the HE image).
 *
 * @retval 0    The SE reports BOOT_CPU succeeded: both the transport-layer
 *              `service_header.error_code` and the service-result
 *              `resp_error_code` came back 0.
 *
 * @retval <0   A LOCAL transport failure: this core's own MHUv2 handshake
 *              with the SE did not complete (e.g. -ETIMEDOUT waiting for the
 *              sender's ACCESS_READY, for the SE to consume the request, or
 *              for the SE's reply doorbell; -EBUSY if the send channel is
 *              still occupied by an unacknowledged prior request; -ENOTCONN
 *              if the SE never answered its readiness heartbeat at all,
 *              which this call always sends first -- distinct from the
 *              other codes above so a bench operator can tell "the SE never
 *              woke" apart from "the SE woke but the boot request itself
 *              timed out/was refused"). No SE-side outcome is known in this
 *              case -- the request may not have reached the SE at all.
 *
 *              CAUTION: after a send-ack timeout specifically, the TX
 *              channel keeps its bits set -- only the SE (the receiver) can
 *              clear them, this core cannot clear its own send. Every later
 *              call to this function then returns -EBUSY permanently, with
 *              no in-band recovery available to this core. Acceptable for a
 *              one-shot boot call; a caller retrying after that particular
 *              failure will not succeed without an SE-side or power-cycle
 *              reset.
 *
 * @retval >0   An SE-REPORTED error: `resp_error_code` (the service-result
 *              field) if it is nonzero, else `service_header.error_code`
 *              (the SE's own transport-layer error field) if THAT is
 *              nonzero. Both fields are logged individually (LOG_ERR)
 *              regardless of which one is returned, so a nonzero value in
 *              the field NOT reflected in the return code is still visible
 *              in the log, never silently dropped.
 */
int alif_se_boot_cpu(uint32_t cpu_id, uint32_t entry_addr);

/**
 * @brief Ask the Alif Secure Enclave to set a target core's vector table
 *        base register (VTOR) before releasing it.
 *
 * Issues Secure-Enclave service_id 505 (SET_VTOR) over the same SE-service
 * MHUv2 transport as alif_se_boot_cpu(), gated behind the same readiness
 * heartbeat, the same `se_boot_lock` mutex, the same stale-RX-doorbell
 * guard, and the same ACCESS_REQUEST deassert cleanup.
 *
 * CONFIRMED WIRE STRUCT, per Alif's DFP se_services headers: SET_VTOR
 * genuinely reuses the same 20-byte request/response wire struct as
 * BOOT_CPU, varying only `header.service_id` -- this is no longer this
 * file's own inference from the vendor's `SERVICES_boot_set_vtor(handle,
 * cpu_id, address, error_code)` wrapper signature (which is the only basis
 * an earlier revision of this comment had), it is what the DFP's own struct
 * definitions show directly. NOTE, also from the DFP: setting VTOR this way
 * only writes a GLOBAL SE-side register, not the target core's own internal
 * VTOR -- see alif_se_reset_cpu() for the step that transfers it, and
 * alif_se_boot_cpu()'s doc comment above for why calling BOOT_CPU alone
 * after this is not enough to make @p vtor_addr take effect. UNVERIFIED ON
 * SILICON like everything else in this module -- see README.md.
 *
 * @param cpu_id    SE-domain CPU id, same domain as alif_se_boot_cpu()'s
 *                  @p cpu_id (ALIF_SE_BOOT_CPU_A32_0/A32_1/M55_HP/M55_HE).
 * @param vtor_addr Vector table BASE address for the target core -- NOT a
 *                  jump target or function pointer. A Cortex-M core fetches
 *                  its initial stack pointer from `[vtor_addr]` and its
 *                  initial program counter from `[vtor_addr + 4]` on
 *                  release; passing a code entry point here (rather than the
 *                  address of the two words a linker script/vector table
 *                  places ahead of it) will make the core load garbage into
 *                  SP and PC.
 *
 * @retval 0   The SE reports SET_VTOR succeeded (same both-fields-zero rule
 *             as alif_se_boot_cpu()'s 0 retval).
 * @retval <0  A LOCAL transport failure, or -ENOTCONN if the SE never
 *             answered the readiness heartbeat this call always sends
 *             first -- see alif_se_boot_cpu()'s retval doc above for the
 *             full breakdown, which applies unchanged here.
 * @retval >0  An SE-REPORTED error (`resp_error_code`, or
 *             `service_header.error_code` if that is the only nonzero
 *             field), clamped to INT_MAX -- see alif_se_boot_cpu()'s retval
 *             doc above.
 */
int alif_se_set_vtor(uint32_t cpu_id, uint32_t vtor_addr);

/**
 * @brief Ask the Alif Secure Enclave to transfer a target core's Global VTOR
 *        register into that core's own internal VTOR register.
 *
 * Issues Secure-Enclave service_id 503 (RESET_CPU) over the same SE-service
 * MHUv2 transport as alif_se_boot_cpu(), gated behind the same readiness
 * heartbeat, mutex, stale-RX-doorbell guard, and ACCESS_REQUEST cleanup.
 *
 * Per Alif's documentation of `SERVICES_boot_reset_cpu()`: this service
 * "stops" the target core (it does not itself release/run it -- see
 * alif_se_release_cpu()) and, for an M55 core specifically, ALSO transfers
 * whatever value a prior alif_se_set_vtor() call wrote into the SE's Global
 * VTOR register into that core's internal VTOR register. Without this call
 * having run, a SET_VTOR alone never reaches the core's own VTOR -- see
 * alif_se_set_vtor()'s doc comment above. Alif documents this transfer;
 * it was NOT observed on AE822FA0E5597LS0, cpu_id 2 (M55_HP), entry
 * 0x50000000, 2026-08-03 -- see docs/BENCH-DUALCORE.md section 3.7. All
 * three calls in that sequence (SET_VTOR/RESET_CPU/RELEASE_CPU) reported
 * success, but the only VTOR reading taken afterwards was post-attach over
 * SWD and therefore confounded by the debugger's own attach-time state
 * clearing -- so that reading does not confirm the transfer happened, and
 * does not disprove it either.
 *
 * Uses a DIFFERENT, 16-byte wire struct from alif_se_boot_cpu()/
 * alif_se_set_vtor()'s 20-byte one -- confirmed by Alif's DFP se_services
 * headers as `control_cpu_svc_t`: header plus `send_cpu_id` plus
 * `resp_error_code`, with no address field (RESET_CPU takes no address of
 * its own; it only moves the value SET_VTOR already staged).
 *
 * UNVERIFIED ON SILICON -- see README.md.
 *
 * @param cpu_id SE-domain CPU id (see alif_se_boot_cpu()'s @p cpu_id).
 *
 * @retval 0   The SE reports RESET_CPU succeeded (same both-fields-zero rule
 *             as alif_se_boot_cpu()'s 0 retval).
 * @retval <0  A LOCAL transport failure, or -ENOTCONN if the SE never
 *             answered the readiness heartbeat -- see alif_se_boot_cpu()'s
 *             retval doc above for the full breakdown.
 * @retval >0  An SE-REPORTED error, clamped to INT_MAX -- see
 *             alif_se_boot_cpu()'s retval doc above.
 */
int alif_se_reset_cpu(uint32_t cpu_id);

/**
 * @brief Ask the Alif Secure Enclave to release (start running) a target
 *        core.
 *
 * Issues Secure-Enclave service_id 502 (RELEASE_CPU) over the same
 * SE-service MHUv2 transport as alif_se_boot_cpu(), with the same gating.
 * Uses the same 16-byte `control_cpu_svc_t`-shaped wire struct as
 * alif_se_reset_cpu() -- see that function's doc comment.
 *
 * This is the step that actually starts the core executing, from whatever
 * its internal VTOR currently points at. Call alif_se_set_vtor() then
 * alif_se_reset_cpu() first if the core needs a specific vector table base
 * -- or call alif_se_start_cpu(), which runs all three in the right order.
 *
 * UNVERIFIED ON SILICON -- see README.md.
 *
 * @param cpu_id SE-domain CPU id (see alif_se_boot_cpu()'s @p cpu_id).
 *
 * @retval 0   The SE reports RELEASE_CPU succeeded (same both-fields-zero
 *             rule as alif_se_boot_cpu()'s 0 retval).
 * @retval <0  A LOCAL transport failure, or -ENOTCONN if the SE never
 *             answered the readiness heartbeat -- see alif_se_boot_cpu()'s
 *             retval doc above for the full breakdown.
 * @retval >0  An SE-REPORTED error, clamped to INT_MAX -- see
 *             alif_se_boot_cpu()'s retval doc above.
 */
int alif_se_release_cpu(uint32_t cpu_id);

/**
 * @brief Ask the Alif Secure Enclave whether it is awake, without issuing
 *        any boot-domain service request.
 *
 * Sends ONLY the maintenance heartbeat (service_id 0) that
 * alif_se_boot_cpu() and alif_se_set_vtor() already send as their first
 * step -- this function exposes that same internal heartbeat path standalone
 * so a caller can probe SE reachability without the side effect of
 * releasing or reconfiguring any core. It takes the same `se_boot_lock`
 * mutex and does the same TX-channel-busy check as those two calls, but
 * builds and sends no BOOT_CPU/SET_VTOR request.
 *
 * @retval 0        The SE acknowledged the heartbeat.
 * @retval -ENOTCONN The SE never answered the heartbeat after the full retry
 *                   budget (same budget alif_se_boot_cpu() uses).
 * @retval <0        Another LOCAL transport failure (e.g. -EBUSY if the
 *                    SE-service TX channel is still occupied by an
 *                    unacknowledged prior request).
 */
int alif_se_ping(void);

/**
 * @brief Correct sequence to start a target core at a specific vector table
 *        base: SET_VTOR, then RESET_CPU, then RELEASE_CPU.
 *
 * Runs, in order, stopping at the first failure:
 *   1. alif_se_set_vtor(cpu_id, entry_addr)  -- SE service_id 505
 *   2. alif_se_reset_cpu(cpu_id)             -- SE service_id 503
 *   3. alif_se_release_cpu(cpu_id)           -- SE service_id 502
 * Each step's outcome is logged distinctly (LOG_INF on success, LOG_ERR on
 * failure, naming the step) so a bench operator can tell which one failed
 * without a debugger.
 *
 * This ordering is not a guess: it is how Alif's own DFP documents these
 * three services' side effects (`services_host_boot.c`'s notes on
 * `SERVICES_boot_set_vtor()` and `SERVICES_boot_reset_cpu()`). SET_VTOR
 * writes only a GLOBAL SE-side VTOR register; RESET_CPU is the step Alif
 * documents as transferring that global value into the target core's own
 * internal VTOR (an M55-specific side effect of RESET_CPU, per the DFP) and
 * stopping the core; RELEASE_CPU is what starts it running. An earlier
 * revision of this function called alif_se_boot_cpu() (service 501,
 * BOOT_CPU) as its second step instead of RESET_CPU/RELEASE_CPU, on a
 * HYPOTHESIS derived only from the SE service enum's ordering -- that guess
 * is what a bench run caught: the released core came up with
 * VTOR == 0x00000000, fetched its initial SP/PC from its own local address
 * 0, and locked up (CFSR == 0x00000001 IACCVIOL, HFSR == 0x40000000
 * FORCED). BOOT_CPU alone never transfers the global VTOR to the core, no
 * matter what ordering it is called in -- see alif_se_boot_cpu()'s doc
 * comment above; Alif's documentation names RESET_CPU as the step that does
 * that instead.
 *
 * UNVERIFIED ON SILICON in this exact three-call form -- see README.md. It
 * rests on Alif's own documented service behaviour rather than this
 * module's earlier enum-ordering guess. As of 2026-08-03 this exact
 * sequence WAS run on E1M-AEN801 (AE822FA0E5597LS0, cpu_id 2 / M55_HP,
 * entry 0x50000000): all three calls returned success, but the documented
 * SET_VTOR -> RESET_CPU VTOR transfer was not independently confirmed --
 * the only VTOR reading taken was post-attach over SWD and is confounded by
 * the debugger's own attach-time state clearing. See
 * docs/BENCH-DUALCORE.md section 3.7 for the full measurement.
 *
 * @param cpu_id     SE-domain CPU id (see alif_se_boot_cpu()'s @p cpu_id).
 * @param entry_addr Vector table BASE address for the target core -- see
 *                    alif_se_set_vtor()'s @p vtor_addr doc: NOT a jump
 *                    target, the core fetches SP from `[entry_addr]` and PC
 *                    from `[entry_addr + 4]`. Passed unchanged to the
 *                    SET_VTOR step only -- RESET_CPU and RELEASE_CPU take no
 *                    address of their own.
 *
 * @retval 0   All three steps succeeded.
 * @retval <0  A LOCAL transport failure from whichever step failed first
 *             (including -ENOTCONN from that step's own heartbeat gate).
 * @retval >0  An SE-REPORTED error from whichever step failed first.
 */
int alif_se_start_cpu(uint32_t cpu_id, uint32_t entry_addr);

#ifdef __cplusplus
}
#endif

#endif /* ALP_LAB_ALIF_SE_BOOT_H_ */
