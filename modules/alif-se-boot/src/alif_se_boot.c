// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2026 Alp Lab AB
 *
 * Client for the Alif Secure Enclave's BOOT_CPU service (service_id 501),
 * used on Alif E8 (Ensemble) silicon to release a secondary RTSS core from a
 * core that already has SE access. On the E1M-AEN801 bench unit that is
 * RTSS-HE releasing RTSS-HP -- see docs/BENCH-DUALCORE.md section 0; which
 * cluster has SE access is a per-silicon boot-order fact, not always
 * RTSS-HP.
 *
 * ============================== PROVENANCE ==============================
 * The wire layouts, field order, and service ids used below (service_id
 * 501/502/503/505, the SE service request/response struct shape, the
 * MHU-based transport sequence, the SET_VTOR/RESET_CPU/RELEASE_CPU call
 * ordering) are interoperability facts read from Alif's DFP se_services
 * headers: they describe the on-the-wire protocol the SE expects and cannot
 * be expressed differently and still work. No code, comment text, or doc
 * prose from those headers was copied into this file -- every function body
 * and comment below is this project's own words. Worth noting separately:
 * three of Alif's se_services headers carry an SPDX-License-Identifier:
 * Apache-2.0 line whose body text then asserts an "All Rights Reserved /
 * Alif Semiconductor Software License Agreement" -- a self-contradictory
 * pairing, noted here as a factual observation about those headers, not a
 * claim about this file.
 *
 * The MHUv2 register offsets used here (channel-window SET/STAT/CLEAR,
 * ACCESS_REQUEST/ACCESS_READY) are the same ARM DDI 0515 facts already
 * transcribed and bench-validated for a DIFFERENT frame pair by the sibling
 * modules/alif-mhuv2 module -- see that module's mbox_alif_mhuv2.c file
 * header for the primary spec citation. This file re-derives them under its
 * own names because it talks to a SEPARATE, SE-dedicated MHUv2 frame pair
 * (mhu@40040000 rx / mhu@40050000 tx) that the doorbell-only alif_mhuv2
 * driver does not own and cannot carry a payload for (see the design-
 * decision note below).
 *
 * UNVERIFIED ON SILICON. Nothing in this file has been exercised on real
 * Alif E8 hardware. See modules/alif-se-boot/README.md for what would need
 * to be true on the bench for a first attempt (in particular: the target
 * core's image must already be resident at its entry address via the SES
 * ATOC "load" mechanism -- this client does not place it there).
 * ==========================================================================
 *
 * ============================ DESIGN DECISION ============================
 * (b) chosen: this file implements its own small, self-contained MHUv2
 * transport, touching the SE-service frame's registers directly, rather
 * than (a) extending the sibling alif_mhuv2 MBOX driver to carry a payload.
 *
 * Why: the alif_mhuv2 driver is deliberately doorbell-only (see its file
 * header: "mtu_get returns 0... no payload travels through the MHU") and is
 * bench-proven on E1M-AEN801 for the RPMsg vring-doorbell path. Teaching it
 * a second mode -- accepting a non-NULL mbox_msg and writing a 32-bit value
 * into CH0_SET instead of just a bit -- would touch send()'s and
 * set_enabled()'s contract for EVERY consumer of that driver (including the
 * proven RPMsg path), for the benefit of exactly one caller (this file) that
 * needs the payload variant on a physically different frame pair anyway.
 * That is a correctness risk on a bench-validated component for no shared
 * gain: this client's frame pair (0x40040000/0x40050000) and the doorbell
 * pair (0x400a0000/0x400b0000) never share a device instance, so there is no
 * code to actually reuse by going through the MBOX class API here -- only
 * register-offset *values*, which are plain hardware facts and cost nothing
 * to re-declare under this file's own names. Keeping this transport
 * self-contained also means a bug or a future change here can never
 * regress modules/alif-mhuv2's bench-proven behaviour.
 * ==========================================================================
 */

#define DT_DRV_COMPAT alplab_e8_se_boot

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <alif_se_boot.h>

LOG_MODULE_REGISTER(alif_se_boot, LOG_LEVEL_INF);

#if DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) != 1
#error "alif_se_boot expects exactly one enabled 'alplab,e8-se-boot' node"
#endif

/*
 * SE-service MHUv2 frame registers (window 0 only), transcribed from ARM DDI
 * 0515 -- see the file header above for why these are re-declared here
 * rather than shared with modules/alif-mhuv2. No offset below is invented;
 * every one of them is also independently present (under different names)
 * in that sibling module's bench-validated driver.
 */
#define SE_MHU_TX_CH0_STAT 0x000U        /* RO  current asserted-bits value      */
#define SE_MHU_TX_CH0_SET 0x00CU         /* RW  OR this value into CH0_STAT      */
#define SE_MHU_TX_ACCESS_REQUEST 0xF88U  /* RW  write 1 to request rx wake       */
#define SE_MHU_TX_ACCESS_READY 0xF8CU    /* RO  reads 1 once rx is awake         */

#define SE_MHU_RX_CH0_STAT 0x000U  /* RO  current asserted-bits value (the payload) */
#define SE_MHU_RX_CH0_CLEAR 0x008U /* W1C write back the bits to release/ack        */

/*
 * Bounded spin budgets. These are SOFTWARE timeouts, not hardware constants
 * -- no datasheet specifies an SE turnaround time, so each budget below is a
 * deliberately generous, arbitrary loop count chosen the same way the
 * sibling alif_mhuv2 driver bounds its own ACCESS_REQUEST/ACCESS_READY wait
 * (MHUV2_ACCESS_READY_SPINS), so a dead or absent SE cannot hang the caller
 * forever. SE_REPLY_SPINS is larger than the other two because it is the
 * only wait that depends on SE firmware actually executing a service
 * request, not just an on-die register propagating.
 */
#define SE_MHU_ACCESS_READY_SPINS 100000U
#define SE_MHU_SEND_ACK_SPINS 100000U
#define SE_MHU_REPLY_SPINS 1000000U

/*
 * The 20-byte SE service request/response structure, per the protocol brief:
 * an 8-byte service header (id/flags/error_code/reserved, all uint16_t) and
 * a 12-byte body (three uint32_t). Every field is naturally aligned already,
 * so __packed below only documents "no padding, ABI-fixed" -- it changes no
 * offset on this target.
 */
struct alif_se_service_header {
	uint16_t          service_id;
	uint16_t          flags;
	volatile uint16_t error_code; /* SE-written: transport-layer error. volatile: this
					* buffer is written by another bus master (the SE), so
					* correctness of the read at the bottom of
					* alif_se_boot_cpu() must not rest on
					* barrier_dmem_fence_full()'s compiler-clobber alone
					* (MINOR 5) -- that is a compiler-dependent guarantee for
					* a field a store-forwarding pass could otherwise elide.
					*/
	uint16_t          reserved;
} __packed;

struct alif_se_boot_svc_body {
	uint32_t          send_cpu_id;
	uint32_t          send_address;
	volatile uint32_t resp_error_code; /* SE-written: service-result error; volatile for
					     * the same reason as header.error_code above. */
} __packed;

/*
 * Shared 20-byte wire shape for BOOT_CPU (501) and SET_VTOR (505) -- both
 * CONFIRMED by Alif's DFP se_services headers to use this same shape
 * (`boot_cpu_svc_t`: header plus `send_cpu_id` plus `send_address` plus
 * `resp_error_code`), varying only `header.service_id`. An earlier revision
 * of this file could only INFER that SET_VTOR reused BOOT_CPU's struct, from
 * the vendor's declared wrapper signatures sharing the same
 * (handle, cpu_id, address, error_code) shape -- see ALIF_SE_SVC_SET_VTOR
 * below and alif_se_set_vtor()'s doc comment in include/alif_se_boot.h for
 * that history. RESET_CPU (503) and RELEASE_CPU (502) do NOT share this
 * shape -- see struct alif_se_control_svc_request below. The heartbeat
 * (se_heartbeat_wait() below) also reuses this struct as an oversized buffer
 * for a smaller `generic_svc_t`-shaped reply -- see that function's comment
 * for why that is safe.
 */
struct alif_se_boot_svc_request {
	struct alif_se_service_header header;
	struct alif_se_boot_svc_body  body;
} __packed;

/*
 * MINOR 4: wire-layout asserts. The only previous BUILD_ASSERT covered the
 * carve-out size, not the layout itself -- nothing failed the build if a
 * field were added, widened, or reordered. Pin both the total size and every
 * SE-visible field's offset so a future edit that changes the wire shape
 * fails to compile instead of silently talking past the SE.
 */
BUILD_ASSERT(sizeof(struct alif_se_boot_svc_request) == 20,
	     "alif_se_boot_svc_request must stay exactly 20 bytes (protocol wire layout)");
BUILD_ASSERT(offsetof(struct alif_se_boot_svc_request, header.service_id) == 0,
	     "header.service_id must stay at offset 0");
BUILD_ASSERT(offsetof(struct alif_se_boot_svc_request, header.flags) == 2,
	     "header.flags must stay at offset 2");
BUILD_ASSERT(offsetof(struct alif_se_boot_svc_request, header.error_code) == 4,
	     "header.error_code must stay at offset 4");
BUILD_ASSERT(offsetof(struct alif_se_boot_svc_request, body.send_cpu_id) == 8,
	     "body.send_cpu_id must stay at offset 8");
BUILD_ASSERT(offsetof(struct alif_se_boot_svc_request, body.send_address) == 12,
	     "body.send_address must stay at offset 12");
BUILD_ASSERT(offsetof(struct alif_se_boot_svc_request, body.resp_error_code) == 16,
	     "body.resp_error_code must stay at offset 16");

/*
 * The 16-byte SE control-CPU request/response structure used by RELEASE_CPU
 * (502) and RESET_CPU (503), CONFIRMED by Alif's DFP se_services headers
 * (`control_cpu_svc_t`): the same 8-byte header as struct
 * alif_se_boot_svc_request above, but only a 4-byte `send_cpu_id` and a
 * 4-byte `resp_error_code` -- NO address field. RESET_CPU and RELEASE_CPU
 * each take only a target cpu_id; neither carries an address of its own.
 */
struct alif_se_control_svc_body {
	uint32_t          send_cpu_id;
	volatile uint32_t resp_error_code; /* SE-written: service-result error; volatile for
					     * the same reason as
					     * alif_se_boot_svc_body.resp_error_code above. */
} __packed;

struct alif_se_control_svc_request {
	struct alif_se_service_header   header;
	struct alif_se_control_svc_body body;
} __packed;

/* MINOR 4, applied to the control-CPU struct too: pin both the total size
 * and every SE-visible field's offset. */
BUILD_ASSERT(sizeof(struct alif_se_control_svc_request) == 16,
	     "alif_se_control_svc_request must stay exactly 16 bytes (protocol wire layout)");
BUILD_ASSERT(offsetof(struct alif_se_control_svc_request, header.service_id) == 0,
	     "header.service_id must stay at offset 0");
BUILD_ASSERT(offsetof(struct alif_se_control_svc_request, header.flags) == 2,
	     "header.flags must stay at offset 2");
BUILD_ASSERT(offsetof(struct alif_se_control_svc_request, header.error_code) == 4,
	     "header.error_code must stay at offset 4");
BUILD_ASSERT(offsetof(struct alif_se_control_svc_request, body.send_cpu_id) == 8,
	     "body.send_cpu_id must stay at offset 8");
BUILD_ASSERT(offsetof(struct alif_se_control_svc_request, body.resp_error_code) == 12,
	     "body.resp_error_code must stay at offset 12");

/*
 * The 20-byte SE PROCESS_TOC_ENTRY (service_id 500) request/response
 * structure, CONFIRMED by Alif's DFP se_services headers
 * (`process_toc_entry_svc_t` in services_lib_protocol.h): the same 8-byte
 * header as every other struct in this file, but an 8-byte `send_entry_id`
 * (the TOC entry's `image_identifier` -- an ASCII name, NOT a cpu_id) in
 * place of BOOT_CPU/SET_VTOR's `send_cpu_id`+`send_address` pair. Same total
 * size as struct alif_se_boot_svc_request (20 bytes) but a DIFFERENT field
 * layout past the header -- this is why PROCESS_TOC_ENTRY needs its own
 * struct rather than reusing either existing one. `ALIF_SE_TOC_ENTRY_ID_LEN`
 * (8) is `IMAGE_NAME_LENGTH` per the DFP header, transcribed here rather than
 * included from it -- see this file's PROVENANCE header for why this module
 * never includes an Alif se_services header directly.
 */
#define ALIF_SE_TOC_ENTRY_ID_LEN 8U

struct alif_se_toc_entry_svc_body {
	uint8_t           send_entry_id[ALIF_SE_TOC_ENTRY_ID_LEN];
	volatile uint32_t resp_error_code; /* SE-written: service-result error; volatile for
					     * the same reason as
					     * alif_se_boot_svc_body.resp_error_code above. */
} __packed;

struct alif_se_toc_entry_svc_request {
	struct alif_se_service_header     header;
	struct alif_se_toc_entry_svc_body body;
} __packed;

/* MINOR 4, applied to the TOC-entry struct too: pin both the total size and
 * every SE-visible field's offset. */
BUILD_ASSERT(sizeof(struct alif_se_toc_entry_svc_request) == 20,
	     "alif_se_toc_entry_svc_request must stay exactly 20 bytes (protocol wire layout)");
BUILD_ASSERT(offsetof(struct alif_se_toc_entry_svc_request, header.service_id) == 0,
	     "header.service_id must stay at offset 0");
BUILD_ASSERT(offsetof(struct alif_se_toc_entry_svc_request, header.flags) == 2,
	     "header.flags must stay at offset 2");
BUILD_ASSERT(offsetof(struct alif_se_toc_entry_svc_request, header.error_code) == 4,
	     "header.error_code must stay at offset 4");
BUILD_ASSERT(offsetof(struct alif_se_toc_entry_svc_request, body.send_entry_id) == 8,
	     "body.send_entry_id must stay at offset 8");
BUILD_ASSERT(offsetof(struct alif_se_toc_entry_svc_request, body.resp_error_code) == 16,
	     "body.resp_error_code must stay at offset 16");

/*
 * service_id for PROCESS_TOC_ENTRY, confirmed by Alif's DFP se_services
 * headers (services_lib_ids.h's `SERVICE_BOOT_START == SERVICE_BOOT_PROCESS_TOC_ENTRY
 * == 500`, the first id in the BOOT service block -- SERVICE_BOOT_CPU (501)
 * immediately follows it). Per services_host_boot.c's doc comment on
 * `SERVICES_boot_process_toc_entry()`: the named TOC entry must already be in
 * a DEFERRED state (not auto-processed by SES at cold boot); this service
 * call un-defers it, which -- depending on that entry's OWN flags (`load`,
 * `boot`) -- can load and/or boot the CPU the entry names. See
 * alif_se_process_toc_entry()'s doc comment in include/alif_se_boot.h for
 * what is and is not yet known about which ATOC JSON field produces the
 * DEFERRED flag.
 */
#define ALIF_SE_SVC_PROCESS_TOC_ENTRY 500U

/* service_id for BOOT_CPU, confirmed by Alif's DFP se_services headers
 * (services_lib_ids.h's SERVICE_BOOT_CPU = 501). */
#define ALIF_SE_SVC_BOOT_CPU 501U

/*
 * service_id for SET_VTOR, confirmed the same way (SERVICE_BOOT_SET_VTOR =
 * 505). The WIRE STRUCT reuse is confirmed too now -- see the comment on
 * struct alif_se_boot_svc_request above.
 */
#define ALIF_SE_SVC_SET_VTOR 505U

/*
 * service_id for RELEASE_CPU, confirmed by Alif's DFP se_services headers
 * (services_lib_ids.h's SERVICE_BOOT_RELEASE_CPU = 502) and by
 * services_lib_protocol.h's dedicated `control_cpu_svc_t` wire struct (see
 * struct alif_se_control_svc_request above). This is the step that actually
 * starts a core running, once RESET_CPU (below) has transferred its VTOR.
 */
#define ALIF_SE_SVC_RELEASE_CPU 502U

/*
 * service_id for RESET_CPU, confirmed the same way (SERVICE_BOOT_RESET_CPU =
 * 503). Per Alif's own documented behaviour of `SERVICES_boot_reset_cpu()`:
 * for an M55 core, this call transfers the value a prior SET_VTOR wrote into
 * the SE's Global VTOR register into that core's own internal VTOR register
 * -- SET_VTOR alone only ever touches the global one. That is the fact this
 * whole fix rests on: see alif_se_start_cpu() below.
 */
#define ALIF_SE_SVC_RESET_CPU 503U

/*
 * MAJOR 2: service_id for the maintenance heartbeat. Per the protocol brief,
 * SERVICE_MAINTENANCE_HEARTBEAT_ID == SERVICE_MAINTENANCE_START == 0 -- the
 * vendor transport gates EVERY service call behind this handshake first
 * ("ensures the Secure Enclave is awake and synchronized, ready to process
 * service requests"). This client issued BOOT_CPU cold before this fix; see
 * se_heartbeat_wait() below.
 */
#define ALIF_SE_SVC_HEARTBEAT 0U

/*
 * Heartbeat retry budget. The vendor retries its 500 ms-timeout heartbeat up
 * to 100 times; this client has no real millisecond figure to reuse (its
 * SE_MHU_REPLY_SPINS budget above is itself an arbitrary software spin
 * count, not a datasheet timing), so it mirrors the vendor's RETRY COUNT
 * (100) rather than inventing a millisecond number this project cannot
 * justify -- each retry reuses the same bounded SE_MHU_* spin budgets as any
 * other transaction, so the worst case is still bounded (never hangs).
 */
#define SE_HEARTBEAT_MAX_RETRIES 100U

/* MINOR 8: one fixed request buffer (SE_REQ_BASE) plus two global MHU
 * register frames -- two concurrent callers would silently corrupt each
 * other's in-flight request without serialization. The vendor transport
 * serializes on a mutex; this client does the same. Cheap for a demo that
 * may grow more than one caller. */
static K_MUTEX_DEFINE(se_boot_lock);

/*
 * The rx/tx SE-MHU frames are SEPARATE devicetree nodes (compatible
 * `alplab,e8-se-mhu-frame`), reached from this node via plain phandle
 * properties rather than a `mboxes` phandle-array -- see the DT binding
 * header comment for why this client bypasses the MBOX class API entirely.
 */
static inline mm_reg_t se_boot_rx_base(void)
{
	return (mm_reg_t)DT_REG_ADDR(DT_INST_PHANDLE(0, alplab_rx_mhu));
}

static inline mm_reg_t se_boot_tx_base(void)
{
	return (mm_reg_t)DT_REG_ADDR(DT_INST_PHANDLE(0, alplab_tx_mhu));
}

/*
 * The request/response structure lives at the base of this node's
 * `memory-region` carve-out (see the DT binding: a small SRAM0 slice that is
 * ALREADY at the same address from every bus master -- no local-to-global
 * address translation is needed, unlike a TCM-resident buffer). This carve-out
 * IS MPU-NOCACHE on this build -- see se_transport_transact()'s flush/
 * invalidate comments for the full verification -- so the flush/invalidate
 * calls around it are defensive, not load-bearing. Its size (20 bytes) is
 * asserted against the carve-out's DT `reg` size at build time below, so a
 * too-small overlay carve-out fails the build instead of silently corrupting
 * an SRAM neighbour. Its alignment (32 bytes, the D-cache line size on this
 * target) is asserted right after, so sys_cache_data_invd_range()'s
 * line-rounding (see se_transport_transact()) can never reach into the
 * preceding line.
 */
#define SE_REQ_BASE DT_REG_ADDR(DT_INST_PHANDLE(0, memory_region))

BUILD_ASSERT(DT_REG_SIZE(DT_INST_PHANDLE(0, memory_region)) >=
		     sizeof(struct alif_se_boot_svc_request),
	     "alplab,e8-se-boot memory-region carve-out is smaller than the SE request struct");

/*
 * MINOR 4 follow-up: alignment, not just size. sys_cache_data_invd_range()
 * (used below) reaches SCB_InvalidateDCache_by_Addr(), which rounds its base
 * address DOWN to the enclosing 32-byte D-cache line before invalidating --
 * an unaligned SE_REQ_BASE would therefore invalidate the tail of whatever
 * line precedes the carve-out too. 0x02020000 happens to satisfy this today
 * (it is 4096-byte aligned), but nothing before this assert enforced it, so a
 * future DT edit that moves the carve-out to an odd offset would silently
 * regress this.
 */
BUILD_ASSERT((SE_REQ_BASE % 32U) == 0U,
	     "alplab,e8-se-boot memory-region carve-out must be 32-byte aligned "
	     "(D-cache line size) so sys_cache_data_invd_range() cannot reach into the preceding line");

/*
 * MINOR 4 follow-up: the flush/invalidate calls in se_transport_transact()
 * are both hardcoded to sizeof(struct alif_se_boot_svc_request) (20 bytes),
 * the largest of the three wire structs built at SE_REQ_BASE
 * (struct alif_se_boot_svc_request itself, 20 bytes; struct
 * alif_se_control_svc_request, 16 bytes; struct alif_se_toc_entry_svc_request,
 * 20 bytes). That single hardcoded size covers all three only because none of
 * them is currently LARGER than alif_se_boot_svc_request -- an accident of
 * today's protocol, not something the code enforces. Assert it so a future,
 * larger wire struct fails the build instead of getting only a partial
 * cache-maintenance range applied to it at runtime.
 */
BUILD_ASSERT(sizeof(struct alif_se_control_svc_request) <=
		     sizeof(struct alif_se_boot_svc_request),
	     "alif_se_control_svc_request must not exceed alif_se_boot_svc_request "
	     "(se_transport_transact()'s cache maintenance is sized off the latter)");
BUILD_ASSERT(sizeof(struct alif_se_toc_entry_svc_request) <=
		     sizeof(struct alif_se_boot_svc_request),
	     "alif_se_toc_entry_svc_request must not exceed alif_se_boot_svc_request "
	     "(se_transport_transact()'s cache maintenance is sized off the latter)");

/*
 * MINOR 7: clamp a raw SE-reported error to a positive `int` instead of
 * casting blindly. A raw value above INT_MAX would flip negative through a
 * naive (int) cast, colliding with this file's own contract
 * (include/alif_se_boot.h): negative is reserved for a LOCAL transport
 * failure, never an SE-reported one. The untouched raw uint32_t is always
 * logged by the caller before this runs, so a clamp never loses the real
 * value -- only the return code's numeric precision, in the (currently
 * undocumented-by-Alif, presumably never-happens) case of an error code that
 * large.
 */
static int se_clamp_positive_error(uint32_t raw)
{
	if (raw > (uint32_t)INT_MAX) {
		LOG_WRN("SE error value 0x%08x exceeds INT_MAX; clamping return to INT_MAX", raw);
		return INT_MAX;
	}
	return (int)raw;
}

/*
 * One SE-service MHUv2 transport transaction against whatever request the
 * caller has already built in place at SE_REQ_BASE: wake, send, wait for
 * send-ack, wait for the reply, drain it. Shared by every caller in this
 * file (heartbeat, BOOT_CPU, SET_VTOR, RESET_CPU, RELEASE_CPU -- MAJOR 2:
 * all must use the SAME transport path, not a duplicated copy). Takes no
 * request-struct pointer: every request this file builds lives at the same
 * fixed SE_REQ_BASE address regardless of its wire shape, and this function
 * never reads or writes request/response FIELDS itself -- only the caller,
 * which already knows which of the two wire shapes it built, does that
 * after this returns 0.
 *
 * Returns 0 once the SE's reply has been received and drained. Returns a
 * negative errno (-ETIMEDOUT) for a LOCAL transport failure -- no SE-side
 * outcome is known in that case, matching this module's documented retval
 * contract. Caller must already hold se_boot_lock.
 */
static int se_transport_transact(mm_reg_t tx, mm_reg_t rx)
{
	uint32_t i;
	uint32_t pending;
	int      ret = 0;

	/*
	 * MAJOR 1: stale-doorbell guard, symmetric with the TX -EBUSY check
	 * the caller already did before this function is entered. If channel
	 * 0 of the SE RX frame already shows a bit set BEFORE we've rung the
	 * doorbell for THIS transaction, it cannot be a reply to anything we
	 * are about to send -- it must be a STALE bit (SE/SES traffic before
	 * Zephyr ran, or a late reply from a previously timed-out call).
	 * Without this guard, the reply-wait loop below would exit on
	 * iteration 0 against a bit this transaction never caused, and the
	 * fields read afterwards would be a leftover from whatever put that
	 * bit there -- a false "success" with the SE never having processed
	 * THIS request. Log distinctly (this is diagnostically valuable on
	 * first bench contact) and drain-and-continue rather than failing the
	 * whole call: a stale bit says nothing about whether the SE can
	 * service THIS transaction, so refusing to even try would be a false
	 * negative on top of the false positive this guards against.
	 */
	pending = sys_read32(rx + SE_MHU_RX_CH0_STAT);
	if (pending != 0U) {
		LOG_WRN("SE-service RX channel had a stale bit (0x%08x) before this request "
			"was sent -- draining it before proceeding",
			pending);
		sys_write32(pending, rx + SE_MHU_RX_CH0_CLEAR);
	}

	/*
	 * (1) Struct is now at an address the SE can read (SE_REQ_BASE is
	 *     inside a `zephyr,memory-region` carve-out -- see the DT binding).
	 *     (2) Flush it out of the D-cache before ringing the SE's doorbell.
	 *
	 * A prior revision of this comment claimed `zephyr,memory-attr =
	 * <(DT_MEM_ARM(ATTR_MPU_RAM_NOCACHE))>` on the overlay's carve-out does
	 * NOT program an MPU region -- reasoning that CONFIG_MEM_ATTR only builds
	 * a lookup API, so the buffer must be cached write-through. That claim was
	 * checked against the actual Zephyr v4.4.0 source, this board's `.config`,
	 * and the linked ELF, and it was WRONG on all three counts:
	 *
	 *   - arch/arm/core/mpu/arm_mpu.c:561-568 calls
	 *     mpu_configure_regions_from_dt() right after the static
	 *     `mpu_config` table is programmed, whenever CONFIG_MEM_ATTR is set --
	 *     DT-declared memory-attr regions ARE programmed into the MPU, on top
	 *     of (not instead of) the static table. `mpu_config.num_regions == 2`
	 *     (FLASH_0, SRAM_0) says nothing about DT-sourced regions, which are
	 *     indexed starting past it.
	 *   - that function (arm_mpu.c:110) is declared `static`, so it carries no
	 *     symbol in `nm` output -- its absence from the symbol table was
	 *     misread as "never linked", when it is simply not externally visible.
	 *     arm_mpu.c:129-131 maps this overlay's `ATTR_MPU_RAM_NOCACHE` tag to
	 *     `REGION_RAM_NOCACHE_ATTR`.
	 *   - subsys/mem_mgmt/Kconfig:4-6 defaults `CONFIG_MEM_ATTR` to y whenever
	 *     `CONFIG_ARM_MPU` is set, and this board's built `.config` for
	 *     e1m_aen/ae822fa0e5597ls0/rtss_he carries CONFIG_ARM_MPU=y,
	 *     CONFIG_MEM_ATTR=y, CONFIG_DCACHE=y, CONFIG_DCACHE_LINE_SIZE=32.
	 *   - decisive: the linked ELF's `mem_attr_region` table (symbol at
	 *     803090e0) holds three 16-byte {name_ptr, addr, size, attr} entries;
	 *     entry 2 is exactly this carve-out --
	 *     addr=0x02020000 size=0x00001000 attr=0x00200000 (sram_se_req) --
	 *     and attr=0x00200000 is DT_MEM_ARM(ATTR_MPU_RAM_NOCACHE).
	 *
	 * So SE_REQ_BASE (0x02020000) IS MPU-NOCACHE at runtime, and this flush is
	 * a no-op on a non-cacheable region today. It is kept anyway: it is
	 * correct and cheap regardless of caching, and it stops being a no-op the
	 * moment this carve-out is ever moved to a cached region or the DT
	 * memory-attr tag is dropped, without anyone having to remember to add it
	 * back. IMPORTANT: because the region is confirmed non-cacheable, this
	 * flush/invalidate pair is NOT what explains the SE-transport -116 /
	 * -ETIMEDOUT failure this module was originally written to chase -- that
	 * failure's actual root cause is NOT established by anything in this file
	 * and may still recur. Do not treat this cache maintenance as a fix for
	 * it.
	 */
	sys_cache_data_flush_range((void *)SE_REQ_BASE, sizeof(struct alif_se_boot_svc_request));

	/* Barrier the caller's stores (and the flush above) so they are
	 * visible before we ring the SE's doorbell below; the store and the
	 * following MMIO write are otherwise free to reorder relative to each
	 * other. */
	barrier_dmem_fence_full();

	/* Wake handshake before ringing: assert ACCESS_REQUEST and spin for
	 * ACCESS_READY, matching the same proven pattern already used for the
	 * doorbell frame pair in modules/alif-mhuv2 (see that driver's
	 * mhuv2_set_enabled()). */
	sys_write32(1U, tx + SE_MHU_TX_ACCESS_REQUEST);
	for (i = 0U; i < SE_MHU_ACCESS_READY_SPINS; i++) {
		if (sys_read32(tx + SE_MHU_TX_ACCESS_READY) != 0U) {
			break;
		}
	}
	if (i == SE_MHU_ACCESS_READY_SPINS) {
		LOG_ERR("SE-service MHU sender never reported ACCESS_READY");
		ret = -ETIMEDOUT;
		goto deassert;
	}

	/*
	 * (4) Send the struct's global address as the message payload: OR the
	 *     full 32-bit SE_REQ_BASE value into the sender's channel-0 STAT
	 *     register via CH0_SET (NOT a single doorbell bit -- the SE reads
	 *     the resulting 32-bit CH_ST value back as the pointer to the
	 *     request it must service).
	 */
	sys_write32((uint32_t)SE_REQ_BASE, tx + SE_MHU_TX_CH0_SET);

	/* (5) Wait for the send acknowledgement: the SE clears CH0_STAT back
	 * to 0 once it has latched the pointer, releasing the channel. */
	for (i = 0U; i < SE_MHU_SEND_ACK_SPINS; i++) {
		if (sys_read32(tx + SE_MHU_TX_CH0_STAT) == 0U) {
			break;
		}
	}
	if (i == SE_MHU_SEND_ACK_SPINS) {
		/* NIT 9: this channel cannot be cleared by the sender, only the
		 * receiver -- see the -EBUSY retval doc in include/alif_se_boot.h
		 * for the permanent--EBUSY-after-this consequence. */
		LOG_ERR("SE never acknowledged the request (send channel still set)");
		ret = -ETIMEDOUT;
		goto deassert;
	}

	/* (6) Wait for the SE's reply on the receive frame: the SE asserts its
	 * own CH0_STAT once the request has been processed and the response
	 * fields in *req have been written back. */
	for (i = 0U; i < SE_MHU_REPLY_SPINS; i++) {
		pending = sys_read32(rx + SE_MHU_RX_CH0_STAT);
		if (pending != 0U) {
			break;
		}
	}
	if (i == SE_MHU_REPLY_SPINS) {
		LOG_ERR("no SE reply within the wait budget");
		ret = -ETIMEDOUT;
		goto deassert;
	}

	/* Ack/clear the reply doorbell so the channel is free for the next
	 * transaction. */
	sys_write32(pending, rx + SE_MHU_RX_CH0_CLEAR);

	/*
	 * (7) Invalidate the request/response buffer before the caller reads
	 * any field the SE wrote back into it (resp_error_code, header.error_code).
	 * Same correction as the comment on the way in above (see
	 * se_transport_transact()'s flush-side comment for the full three-way
	 * verification): this carve-out IS MPU-NOCACHE on this build --
	 * confirmed by Zephyr's own DT-driven MPU configuration and the linked
	 * ELF's `mem_attr_region` table, entry 2 (addr=0x02020000
	 * size=0x00001000 attr=0x00200000, matching DT_MEM_ARM(ATTR_MPU_RAM_NOCACHE))
	 * -- so this invalidate is a no-op on today's build, kept for the same
	 * defensive reason as the flush above: it costs nothing on a
	 * non-cacheable region and stays correct if that ever changes. Mirrors
	 * the vendor transport's post-reply `RTSS_InvalidateDCache_by_Addr()` --
	 * see se_services/source/services_host_handler.c:248-249 in the Alif DFP
	 * reference tree, cited as a fact about the vendor's own transport (see
	 * this file's PROVENANCE header). The barrier below still orders the
	 * RX-doorbell read above against the struct read the caller does next.
	 *
	 * IMPORTANT: because this region is confirmed non-cacheable, this
	 * invalidate is NOT what explains the -116 / -ETIMEDOUT SE-transport
	 * failure this module was originally written to chase. That failure's
	 * root cause remains UNKNOWN -- see the flush-side comment above -- and
	 * this maintenance pair should not be read as having fixed it.
	 */
	sys_cache_data_invd_range((void *)SE_REQ_BASE, sizeof(struct alif_se_boot_svc_request));
	barrier_dmem_fence_full();

deassert:
	/* MINOR 6: ACCESS_REQUEST must be deasserted on EVERY path once it has
	 * been asserted above -- success, ACCESS_READY timeout, send-ack
	 * timeout, or reply timeout alike. ARM's reference MHUv2 driver pairs
	 * initiate-transfer with close-transfer (writing 0 back to this same
	 * register); leaving it asserted pins the SE-side MHU receiver awake
	 * for the life of the image. A single cleanup label covers every
	 * return path above it. */
	sys_write32(0U, tx + SE_MHU_TX_ACCESS_REQUEST);
	return ret;
}

/*
 * MAJOR 2: readiness handshake. The vendor gates EVERY service call behind
 * SERVICE_MAINTENANCE_HEARTBEAT_ID (== SERVICE_MAINTENANCE_START == 0) first,
 * "to ensure the Secure Enclave is awake and synchronized, ready to process
 * service requests" -- retried up to 100 times against a 500 ms timeout per
 * attempt. This client previously issued BOOT_CPU cold; that gap is closed
 * here, unconditionally (not behind a Kconfig opt-in): the heartbeat reuses
 * the exact same bounded, already-reviewed se_transport_transact() path as
 * BOOT_CPU itself, so it carries no additional risk on untested silicon
 * beyond what BOOT_CPU already carries alone.
 *
 * The heartbeat request is `generic_svc_t`-shaped per the protocol brief:
 * an 8-byte header plus a single resp_error_code (12 bytes total) -- smaller
 * than, and NOT layout-compatible past the header with, this file's 20-byte
 * BOOT_CPU-shaped struct (BOOT_CPU's resp_error_code sits at offset 16;
 * generic_svc_t's sits at offset 8). Sending the larger BOOT_CPU-shaped
 * buffer for the heartbeat is fine per the protocol brief AS LONG AS the
 * header fields are correct (service_id = 0, everything else zeroed) -- this
 * function does exactly that, and deliberately does NOT read back
 * req->body.resp_error_code afterwards (that offset does not mean the same
 * thing for this service). Only req->header.error_code is read, which sits
 * at the SAME offset (4) in every SE service reply regardless of body shape.
 */
static int se_heartbeat_wait(mm_reg_t tx, mm_reg_t rx)
{
	struct alif_se_boot_svc_request *req = (struct alif_se_boot_svc_request *)SE_REQ_BASE;
	uint32_t                         attempt;
	int                               ret;

	for (attempt = 0U; attempt < SE_HEARTBEAT_MAX_RETRIES; attempt++) {
		req->header.service_id    = ALIF_SE_SVC_HEARTBEAT;
		req->header.flags         = 0U;
		req->header.error_code    = 0U;
		req->header.reserved      = 0U;
		req->body.send_cpu_id     = 0U;
		req->body.send_address    = 0U;
		req->body.resp_error_code = 0U;

		ret = se_transport_transact(tx, rx);
		if (ret == 0) {
			if (req->header.error_code != 0U) {
				LOG_WRN("SE heartbeat replied with error_code=%u on attempt "
					"%u/%u; treating the SE as awake anyway (a reply of "
					"any kind means it is alive and synchronized)",
					req->header.error_code, attempt + 1,
					SE_HEARTBEAT_MAX_RETRIES);
			}
			LOG_INF("SE heartbeat acknowledged (attempt %u/%u)", attempt + 1,
				SE_HEARTBEAT_MAX_RETRIES);
			return 0;
		}

		LOG_WRN("SE heartbeat attempt %u/%u got no reply (%d); retrying", attempt + 1,
			SE_HEARTBEAT_MAX_RETRIES, ret);
	}

	/*
	 * MAJOR 2: distinct negative errno from every BOOT_CPU-path failure
	 * (-EBUSY / -ETIMEDOUT), so a bench operator can immediately tell "the
	 * SE never woke at all" apart from "the SE woke but refused/timed out
	 * on the boot request itself". -ENOTCONN is chosen deliberately for
	 * that reason -- it is not otherwise used anywhere in this file.
	 */
	return -ENOTCONN;
}

int alif_se_process_toc_entry(const char *image_id)
{
	struct alif_se_toc_entry_svc_request *req = (struct alif_se_toc_entry_svc_request *)SE_REQ_BASE;
	mm_reg_t                              tx  = se_boot_tx_base();
	mm_reg_t                              rx  = se_boot_rx_base();
	int                                   ret;

	/*
	 * Reject a NULL @p image_id outright (strncpy() below would fault on
	 * it) and reject anything longer than ALIF_SE_TOC_ENTRY_ID_LEN (8)
	 * bytes BEFORE it reaches strncpy(). Without this check, a caller
	 * passing a name longer than the wire field silently gets the first 8
	 * bytes copied and the rest dropped -- the SE then processes whatever
	 * OTHER TOC entry happens to share that 8-byte prefix (or none at
	 * all), not the one the caller asked for, with no error raised
	 * anywhere in this call chain. Checked before the mutex/heartbeat so a
	 * doomed call fails fast without waking the SE for nothing.
	 */
	if (image_id == NULL) {
		LOG_ERR("alif_se_process_toc_entry(): image_id is NULL");
		return -EINVAL;
	}
	if (strlen(image_id) > ALIF_SE_TOC_ENTRY_ID_LEN) {
		LOG_ERR("alif_se_process_toc_entry(): image_id \"%s\" is longer than "
			"ALIF_SE_TOC_ENTRY_ID_LEN (%u) bytes -- refusing to silently truncate it",
			image_id, ALIF_SE_TOC_ENTRY_ID_LEN);
		return -EINVAL;
	}

	/* Same gating as every other call in this file -- see
	 * alif_se_boot_cpu()'s comments above for why each step exists. Routed
	 * through the SAME se_transport_transact() path as every other service
	 * (MAJOR 2, this file's own top-of-file comment on
	 * se_transport_transact()) -- no second transport for this service. */
	k_mutex_lock(&se_boot_lock, K_FOREVER);

	if (sys_read32(tx + SE_MHU_TX_CH0_STAT) != 0U) {
		LOG_ERR("SE-service TX channel busy (unacked prior request)");
		k_mutex_unlock(&se_boot_lock);
		return -EBUSY;
	}

	ret = se_heartbeat_wait(tx, rx);
	if (ret != 0) {
		LOG_ERR("SE never woke for its readiness heartbeat (%d) -- "
			"PROCESS_TOC_ENTRY(image_id=%.8s) NOT sent",
			ret, image_id);
		k_mutex_unlock(&se_boot_lock);
		return ret;
	}

	req->header.service_id = ALIF_SE_SVC_PROCESS_TOC_ENTRY;
	req->header.flags      = 0U;
	req->header.error_code = 0U;
	req->header.reserved   = 0U;

	/*
	 * Same strncpy() semantics Alif's own SERVICES_boot_process_toc_entry()
	 * wrapper uses (per services_host_boot.c, cited as a fact about the
	 * vendor's own transport -- see this file's PROVENANCE header, no code
	 * copied): copies at most ALIF_SE_TOC_ENTRY_ID_LEN bytes, zero-padding
	 * any remainder. `send_entry_id` is the TOC's raw fixed-width 8-byte
	 * `image_identifier` field, NOT a C string -- if @p image_id is exactly
	 * 8 bytes or longer, the field intentionally ends up WITHOUT a NUL
	 * terminator (matching the TOC entry name format), so this field must
	 * never be read back with a plain %s.
	 */
	strncpy((char *)req->body.send_entry_id, image_id, ALIF_SE_TOC_ENTRY_ID_LEN);
	req->body.resp_error_code = 0U;

	ret = se_transport_transact(tx, rx);
	if (ret != 0) {
		k_mutex_unlock(&se_boot_lock);
		return ret;
	}

	if (req->header.error_code != 0U) {
		LOG_ERR("SE transport-layer error_code=%u for PROCESS_TOC_ENTRY(image_id=%.8s)",
			req->header.error_code, image_id);
	}
	if (req->body.resp_error_code != 0U) {
		LOG_ERR("SE service error resp_error_code=%u for PROCESS_TOC_ENTRY(image_id=%.8s)",
			req->body.resp_error_code, image_id);
	}

	if (req->header.error_code == 0U && req->body.resp_error_code == 0U) {
		LOG_INF("SE PROCESS_TOC_ENTRY(image_id=%.8s) succeeded (entry un-deferred)",
			image_id);
		k_mutex_unlock(&se_boot_lock);
		return 0;
	}

	k_mutex_unlock(&se_boot_lock);

	return (req->body.resp_error_code != 0U)
		       ? se_clamp_positive_error(req->body.resp_error_code)
		       : se_clamp_positive_error(req->header.error_code);
}

int alif_se_boot_cpu(uint32_t cpu_id, uint32_t entry_addr)
{
	struct alif_se_boot_svc_request *req = (struct alif_se_boot_svc_request *)SE_REQ_BASE;
	mm_reg_t                         tx  = se_boot_tx_base();
	mm_reg_t                         rx  = se_boot_rx_base();
	int                               ret;

	/* MINOR 8: serialize the whole call -- SE_REQ_BASE and both MHU frames
	 * are shared, global state; two concurrent callers must not interleave
	 * their transactions against them. */
	k_mutex_lock(&se_boot_lock, K_FOREVER);

	/* Refuse to start a new request while the SE-service channel still
	 * shows an unacknowledged prior payload -- that would silently clobber
	 * whatever request is still in flight. */
	if (sys_read32(tx + SE_MHU_TX_CH0_STAT) != 0U) {
		LOG_ERR("SE-service TX channel busy (unacked prior request)");
		k_mutex_unlock(&se_boot_lock);
		return -EBUSY;
	}

	/* MAJOR 2: SE readiness gate, before touching BOOT_CPU at all. */
	ret = se_heartbeat_wait(tx, rx);
	if (ret != 0) {
		LOG_ERR("SE never woke for its readiness heartbeat (%d) -- BOOT_CPU(cpu_id=%u) "
			"NOT sent",
			ret, cpu_id);
		k_mutex_unlock(&se_boot_lock);
		return ret;
	}

	/* Build the BOOT_CPU request in place. Step numbering in
	 * se_transport_transact() matches the transport sequence in the
	 * protocol brief. */
	req->header.service_id    = ALIF_SE_SVC_BOOT_CPU;
	req->header.flags         = 0U;
	req->header.error_code    = 0U;
	req->header.reserved      = 0U;
	req->body.send_cpu_id     = cpu_id;
	req->body.send_address    = entry_addr;
	req->body.resp_error_code = 0U;

	ret = se_transport_transact(tx, rx);
	if (ret != 0) {
		k_mutex_unlock(&se_boot_lock);
		return ret;
	}

	if (req->header.error_code != 0U) {
		LOG_ERR("SE transport-layer error_code=%u for BOOT_CPU(cpu_id=%u)",
			req->header.error_code, cpu_id);
	}
	if (req->body.resp_error_code != 0U) {
		LOG_ERR("SE service error resp_error_code=%u for BOOT_CPU(cpu_id=%u)",
			req->body.resp_error_code, cpu_id);
	}

	if (req->header.error_code == 0U && req->body.resp_error_code == 0U) {
		LOG_INF("SE BOOT_CPU(cpu_id=%u, entry=0x%08x) succeeded", cpu_id, entry_addr);
		k_mutex_unlock(&se_boot_lock);
		return 0;
	}

	k_mutex_unlock(&se_boot_lock);

	/* Positive SE-reported error: resp_error_code (the service result)
	 * takes precedence when both fields are nonzero; error_code is still
	 * logged above regardless, so it is never silently dropped. MINOR 7:
	 * clamp rather than cast, so a value above INT_MAX cannot flip
	 * negative and collide with this function's LOCAL-failure retval
	 * class. */
	return (req->body.resp_error_code != 0U)
		       ? se_clamp_positive_error(req->body.resp_error_code)
		       : se_clamp_positive_error(req->header.error_code);
}

int alif_se_set_vtor(uint32_t cpu_id, uint32_t vtor_addr)
{
	struct alif_se_boot_svc_request *req = (struct alif_se_boot_svc_request *)SE_REQ_BASE;
	mm_reg_t                         tx  = se_boot_tx_base();
	mm_reg_t                         rx  = se_boot_rx_base();
	int                               ret;

	/* Same gating as alif_se_boot_cpu() -- see its comments above for why
	 * each step exists. This function is structurally identical to
	 * alif_se_boot_cpu(), differing only in the service_id sent and the
	 * log labels, per this file's CONFIRMED fact that SET_VTOR reuses the
	 * BOOT_CPU wire struct (see struct alif_se_boot_svc_request's comment
	 * and ALIF_SE_SVC_SET_VTOR's comment above). */
	k_mutex_lock(&se_boot_lock, K_FOREVER);

	if (sys_read32(tx + SE_MHU_TX_CH0_STAT) != 0U) {
		LOG_ERR("SE-service TX channel busy (unacked prior request)");
		k_mutex_unlock(&se_boot_lock);
		return -EBUSY;
	}

	ret = se_heartbeat_wait(tx, rx);
	if (ret != 0) {
		LOG_ERR("SE never woke for its readiness heartbeat (%d) -- SET_VTOR(cpu_id=%u) "
			"NOT sent",
			ret, cpu_id);
		k_mutex_unlock(&se_boot_lock);
		return ret;
	}

	req->header.service_id    = ALIF_SE_SVC_SET_VTOR;
	req->header.flags         = 0U;
	req->header.error_code    = 0U;
	req->header.reserved      = 0U;
	req->body.send_cpu_id     = cpu_id;
	req->body.send_address    = vtor_addr;
	req->body.resp_error_code = 0U;

	ret = se_transport_transact(tx, rx);
	if (ret != 0) {
		k_mutex_unlock(&se_boot_lock);
		return ret;
	}

	if (req->header.error_code != 0U) {
		LOG_ERR("SE transport-layer error_code=%u for SET_VTOR(cpu_id=%u)",
			req->header.error_code, cpu_id);
	}
	if (req->body.resp_error_code != 0U) {
		LOG_ERR("SE service error resp_error_code=%u for SET_VTOR(cpu_id=%u)",
			req->body.resp_error_code, cpu_id);
	}

	if (req->header.error_code == 0U && req->body.resp_error_code == 0U) {
		LOG_INF("SE SET_VTOR(cpu_id=%u, vtor=0x%08x) succeeded", cpu_id, vtor_addr);
		k_mutex_unlock(&se_boot_lock);
		return 0;
	}

	k_mutex_unlock(&se_boot_lock);

	return (req->body.resp_error_code != 0U)
		       ? se_clamp_positive_error(req->body.resp_error_code)
		       : se_clamp_positive_error(req->header.error_code);
}

int alif_se_reset_cpu(uint32_t cpu_id)
{
	struct alif_se_control_svc_request *req = (struct alif_se_control_svc_request *)SE_REQ_BASE;
	mm_reg_t                            tx  = se_boot_tx_base();
	mm_reg_t                            rx  = se_boot_rx_base();
	int                                 ret;

	/* Same gating as alif_se_boot_cpu() -- see its comments above for why
	 * each step exists. Builds the 16-byte control-CPU struct instead of
	 * the 20-byte boot-CPU one -- see struct alif_se_control_svc_request's
	 * comment above for why RESET_CPU/RELEASE_CPU carry no address. */
	k_mutex_lock(&se_boot_lock, K_FOREVER);

	if (sys_read32(tx + SE_MHU_TX_CH0_STAT) != 0U) {
		LOG_ERR("SE-service TX channel busy (unacked prior request)");
		k_mutex_unlock(&se_boot_lock);
		return -EBUSY;
	}

	ret = se_heartbeat_wait(tx, rx);
	if (ret != 0) {
		LOG_ERR("SE never woke for its readiness heartbeat (%d) -- RESET_CPU(cpu_id=%u) "
			"NOT sent",
			ret, cpu_id);
		k_mutex_unlock(&se_boot_lock);
		return ret;
	}

	req->header.service_id    = ALIF_SE_SVC_RESET_CPU;
	req->header.flags         = 0U;
	req->header.error_code    = 0U;
	req->header.reserved      = 0U;
	req->body.send_cpu_id     = cpu_id;
	req->body.resp_error_code = 0U;

	ret = se_transport_transact(tx, rx);
	if (ret != 0) {
		k_mutex_unlock(&se_boot_lock);
		return ret;
	}

	if (req->header.error_code != 0U) {
		LOG_ERR("SE transport-layer error_code=%u for RESET_CPU(cpu_id=%u)",
			req->header.error_code, cpu_id);
	}
	if (req->body.resp_error_code != 0U) {
		LOG_ERR("SE service error resp_error_code=%u for RESET_CPU(cpu_id=%u)",
			req->body.resp_error_code, cpu_id);
	}

	if (req->header.error_code == 0U && req->body.resp_error_code == 0U) {
		LOG_INF("SE RESET_CPU(cpu_id=%u) succeeded (global VTOR transferred to the core's "
			"internal VTOR)",
			cpu_id);
		k_mutex_unlock(&se_boot_lock);
		return 0;
	}

	k_mutex_unlock(&se_boot_lock);

	return (req->body.resp_error_code != 0U)
		       ? se_clamp_positive_error(req->body.resp_error_code)
		       : se_clamp_positive_error(req->header.error_code);
}

int alif_se_release_cpu(uint32_t cpu_id)
{
	struct alif_se_control_svc_request *req = (struct alif_se_control_svc_request *)SE_REQ_BASE;
	mm_reg_t                            tx  = se_boot_tx_base();
	mm_reg_t                            rx  = se_boot_rx_base();
	int                                 ret;

	/* Same gating and 16-byte control-CPU struct as alif_se_reset_cpu()
	 * above -- see its comments for why each step exists. */
	k_mutex_lock(&se_boot_lock, K_FOREVER);

	if (sys_read32(tx + SE_MHU_TX_CH0_STAT) != 0U) {
		LOG_ERR("SE-service TX channel busy (unacked prior request)");
		k_mutex_unlock(&se_boot_lock);
		return -EBUSY;
	}

	ret = se_heartbeat_wait(tx, rx);
	if (ret != 0) {
		LOG_ERR("SE never woke for its readiness heartbeat (%d) -- RELEASE_CPU(cpu_id=%u) "
			"NOT sent",
			ret, cpu_id);
		k_mutex_unlock(&se_boot_lock);
		return ret;
	}

	req->header.service_id    = ALIF_SE_SVC_RELEASE_CPU;
	req->header.flags         = 0U;
	req->header.error_code    = 0U;
	req->header.reserved      = 0U;
	req->body.send_cpu_id     = cpu_id;
	req->body.resp_error_code = 0U;

	ret = se_transport_transact(tx, rx);
	if (ret != 0) {
		k_mutex_unlock(&se_boot_lock);
		return ret;
	}

	if (req->header.error_code != 0U) {
		LOG_ERR("SE transport-layer error_code=%u for RELEASE_CPU(cpu_id=%u)",
			req->header.error_code, cpu_id);
	}
	if (req->body.resp_error_code != 0U) {
		LOG_ERR("SE service error resp_error_code=%u for RELEASE_CPU(cpu_id=%u)",
			req->body.resp_error_code, cpu_id);
	}

	if (req->header.error_code == 0U && req->body.resp_error_code == 0U) {
		LOG_INF("SE RELEASE_CPU(cpu_id=%u) succeeded", cpu_id);
		k_mutex_unlock(&se_boot_lock);
		return 0;
	}

	k_mutex_unlock(&se_boot_lock);

	return (req->body.resp_error_code != 0U)
		       ? se_clamp_positive_error(req->body.resp_error_code)
		       : se_clamp_positive_error(req->header.error_code);
}

int alif_se_ping(void)
{
	mm_reg_t tx = se_boot_tx_base();
	mm_reg_t rx = se_boot_rx_base();
	int      ret;

	/* Same mutex + stale-request-buffer guard as alif_se_boot_cpu() /
	 * alif_se_set_vtor() (se_heartbeat_wait() writes its own request into
	 * the same shared SE_REQ_BASE buffer), but sends nothing beyond the
	 * heartbeat itself -- no BOOT_CPU/SET_VTOR request is built or sent,
	 * so a probe call cannot release or reconfigure any core as a side
	 * effect. */
	k_mutex_lock(&se_boot_lock, K_FOREVER);

	if (sys_read32(tx + SE_MHU_TX_CH0_STAT) != 0U) {
		LOG_ERR("SE-service TX channel busy (unacked prior request) -- ping NOT sent");
		k_mutex_unlock(&se_boot_lock);
		return -EBUSY;
	}

	ret = se_heartbeat_wait(tx, rx);

	k_mutex_unlock(&se_boot_lock);
	return ret;
}

int alif_se_start_cpu(uint32_t cpu_id, uint32_t entry_addr)
{
	int ret;

	/*
	 * SET_VTOR -> RESET_CPU -> RELEASE_CPU, per Alif's own DFP
	 * documentation of what each service does (see this function's doc
	 * comment in include/alif_se_boot.h for the full account) -- not the
	 * SET_VTOR-then-BOOT_CPU enum-ordering guess an earlier revision of
	 * this function used, which is what a bench run caught: BOOT_CPU never
	 * transfers the global VTOR SET_VTOR staged into the released core's
	 * own internal VTOR, only RESET_CPU does that.
	 */
	ret = alif_se_set_vtor(cpu_id, entry_addr);
	if (ret != 0) {
		LOG_ERR("alif_se_start_cpu(cpu_id=%u): SET_VTOR step failed (%d) -- RESET_CPU/"
			"RELEASE_CPU steps NOT attempted",
			cpu_id, ret);
		return ret;
	}
	LOG_INF("alif_se_start_cpu(cpu_id=%u): SET_VTOR step succeeded", cpu_id);

	ret = alif_se_reset_cpu(cpu_id);
	if (ret != 0) {
		LOG_ERR("alif_se_start_cpu(cpu_id=%u): RESET_CPU step failed (%d) -- RELEASE_CPU "
			"step NOT attempted",
			cpu_id, ret);
		return ret;
	}
	LOG_INF("alif_se_start_cpu(cpu_id=%u): RESET_CPU step succeeded (global VTOR transferred "
		"to the core's internal VTOR)",
		cpu_id);

	ret = alif_se_release_cpu(cpu_id);
	if (ret != 0) {
		LOG_ERR("alif_se_start_cpu(cpu_id=%u): RELEASE_CPU step failed (%d)", cpu_id, ret);
		return ret;
	}
	LOG_INF("alif_se_start_cpu(cpu_id=%u): RELEASE_CPU step succeeded", cpu_id);

	return 0;
}
