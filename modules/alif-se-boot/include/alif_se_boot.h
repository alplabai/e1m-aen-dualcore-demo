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
 * transcribed in the calling task's protocol brief. Only M55_HE is exercised
 * by this repo (apps/dualcore_hp releasing apps/dualcore_he); the other
 * three are listed here only so a caller never has to invent or guess one.
 */
#define ALIF_SE_BOOT_CPU_A32_0 0U
#define ALIF_SE_BOOT_CPU_A32_1 1U
#define ALIF_SE_BOOT_CPU_M55_HP 2U
#define ALIF_SE_BOOT_CPU_M55_HE 3U

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
 * INFERENCE, NOT A CONFIRMED WIRE STRUCT: no dedicated `set_vtor_svc_t` was
 * found in the transcribed SE protocol. The vendor's declared wrapper,
 * `SERVICES_boot_set_vtor(services_handle, cpu_id, address, error_code)`,
 * has the identical `(handle, cpu_id, address, error_code)` shape as
 * `SERVICES_boot_cpu()`, so this client reuses the same 20-byte request/
 * response wire struct as alif_se_boot_cpu(), varying only
 * `header.service_id`. That reuse is this file's own inference from the
 * wrapper signature, not a fact confirmed anywhere in the protocol
 * transcription. UNVERIFIED ON SILICON like everything else in this module
 * -- see README.md.
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
 * @brief Convenience wrapper: SET_VTOR then BOOT_CPU, in that order,
 *        stopping at the first failure.
 *
 * Runs, in order:
 *   1. alif_se_set_vtor(cpu_id, entry_addr)  -- SE service_id 505
 *   2. alif_se_boot_cpu(cpu_id, entry_addr)  -- SE service_id 501
 * and returns the first nonzero result without attempting the next step.
 * Each step's outcome is logged distinctly (LOG_INF on success, LOG_ERR on
 * failure, naming the step) so a bench operator can tell which one failed.
 *
 * HYPOTHESIS, NOT CONFIRMED ON SILICON: this step ordering is derived from
 * the SE service enum (SET_VTOR sits between BOOT_START/PROCESS_TOC_ENTRY
 * and BOOT_CPU in the transcribed protocol) and from a bench failure in
 * which a released core came up with VTOR == 0x00000000, fetched its
 * initial SP/PC from its own local address 0, and locked up
 * (CFSR == 0x00000001 IACCVIOL, HFSR == 0x40000000 FORCED) -- i.e.
 * `alif_se_boot_cpu()`'s `send_address` did NOT become the released core's
 * vector table base on that attempt. This function's ordering has NOT
 * itself been exercised on hardware. `SERVICE_BOOT_RELEASE_CPU` (502) is an
 * UNTRIED alternative to step 2 that this function does not use.
 *
 * @param cpu_id     SE-domain CPU id (see alif_se_boot_cpu()'s @p cpu_id).
 * @param entry_addr Vector table BASE address for the target core -- see
 *                    alif_se_set_vtor()'s @p vtor_addr doc: NOT a jump
 *                    target, the core fetches SP from `[entry_addr]` and PC
 *                    from `[entry_addr + 4]`. Passed unchanged to both the
 *                    SET_VTOR and BOOT_CPU steps.
 *
 * @retval 0   Both steps succeeded.
 * @retval <0  A LOCAL transport failure from whichever step failed first
 *             (including -ENOTCONN from that step's own heartbeat gate).
 * @retval >0  An SE-REPORTED error from whichever step failed first.
 */
int alif_se_start_cpu(uint32_t cpu_id, uint32_t entry_addr);

#ifdef __cplusplus
}
#endif

#endif /* ALP_LAB_ALIF_SE_BOOT_H_ */
