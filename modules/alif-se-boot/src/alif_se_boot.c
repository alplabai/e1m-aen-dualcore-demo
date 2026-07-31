// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2026 Alp Lab AB
 *
 * Client for the Alif Secure Enclave's BOOT_CPU service (service_id 501),
 * used on Alif E8 (Ensemble) silicon to release a secondary RTSS core (e.g.
 * RTSS-HE) from a core that already has SE access (RTSS-HP).
 *
 * ============================== PROVENANCE ==============================
 * AUTHORED FROM THE PROTOCOL, NOT FROM ALIF'S SOURCE. This file was written
 * against a transcription of the SE service request/response layout and the
 * MHU-based transport sequence -- it does not copy, adapt, or lift any code
 * or text from Alif's se_services sources. That matters here specifically:
 * three of Alif's se_services headers carry an SPDX-License-Identifier:
 * Apache-2.0 line whose body text then asserts an "All Rights Reserved /
 * Alif Semiconductor Software License Agreement" -- a self-contradictory
 * pairing this project does not want to inherit by copying from it. Every
 * type, constant, and function body below is this project's own.
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
 * Shared 20-byte wire shape for every boot-domain service this file speaks:
 * BOOT_CPU (501) confirmed by the protocol brief, and SET_VTOR (505) by
 * INFERENCE (see ALIF_SE_SVC_SET_VTOR below and alif_se_set_vtor()'s doc
 * comment in include/alif_se_boot.h -- no dedicated `set_vtor_svc_t` was
 * found in the transcribed protocol; this reuses the BOOT_CPU struct shape
 * because the vendor's declared wrappers for both services share the same
 * (handle, cpu_id, address, error_code) signature). The heartbeat
 * (se_heartbeat_wait() below) also reuses this struct as an oversized
 * buffer for a smaller `generic_svc_t`-shaped reply -- see that function's
 * comment for why that is safe.
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

/* service_id for BOOT_CPU, per the protocol brief. This client implements
 * only this one service; PROCESS_TOC_ENTRY (500) / RELEASE_CPU (502) /
 * RESET_CPU (503) are documented facts about the same service class but are
 * out of scope for this file. */
#define ALIF_SE_SVC_BOOT_CPU 501U

/*
 * service_id for SET_VTOR, per the protocol brief's service enum
 * (SERVICE_BOOT_START=500 .. SERVICE_BOOT_END=599, SET_VTOR at 505, between
 * RESET_CPU=503/RESET_SOC=504 and SET_ARGS=506). INFERENCE about the WIRE
 * STRUCT only, not about this numeric id: see the comment on
 * struct alif_se_boot_svc_request above.
 */
#define ALIF_SE_SVC_SET_VTOR 505U

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
 * `memory-region` carve-out (see the DT binding: a small, non-cacheable
 * SRAM0 slice that is ALREADY at the same address from every bus master --
 * no local-to-global address translation is needed, unlike a TCM-resident
 * buffer). Its size (20 bytes) is asserted against the carve-out's DT `reg`
 * size at build time below, so a too-small overlay carve-out fails the build
 * instead of silently corrupting an SRAM neighbour.
 */
#define SE_REQ_BASE DT_REG_ADDR(DT_INST_PHANDLE(0, memory_region))

BUILD_ASSERT(DT_REG_SIZE(DT_INST_PHANDLE(0, memory_region)) >=
		     sizeof(struct alif_se_boot_svc_request),
	     "alplab,e8-se-boot memory-region carve-out is smaller than the SE request struct");

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
 * One SE-service MHUv2 transport transaction against *req (which must
 * already hold a fully-built request at SE_REQ_BASE): wake, send, wait for
 * send-ack, wait for the reply, drain it. Shared by both se_heartbeat_wait()
 * and alif_se_boot_cpu() (MAJOR 2: both must use the SAME transport path, not
 * a duplicated copy). Interprets no reply CONTENT -- heartbeat and BOOT_CPU
 * read *req's fields differently once this returns 0, so that stays the
 * caller's job.
 *
 * Returns 0 once the SE's reply has been received and drained. Returns a
 * negative errno (-ETIMEDOUT) for a LOCAL transport failure -- no SE-side
 * outcome is known in that case, matching this module's documented retval
 * contract. Caller must already hold se_boot_lock.
 */
static int se_transport_transact(struct alif_se_boot_svc_request *req, mm_reg_t tx, mm_reg_t rx)
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
	 *     inside a `zephyr,memory-region` carve-out that is globally
	 *     addressable AND mapped ATTR_MPU_RAM_NOCACHE by the board overlay
	 *     -- see the DT binding). (2) Barrier the caller's stores so they
	 *     are visible before we ring the SE's doorbell below; even on a
	 *     non-cacheable mapping the store and the following MMIO write are
	 *     otherwise free to reorder relative to each other. (3) No D-cache
	 *     flush is issued here -- deliberately: unlike a TCM-resident
	 *     request buffer (which the vendor transport must flush because its
	 *     TCM mapping is cacheable), this carve-out is already
	 *     non-cacheable, so there is no dirty cache line to push out. This
	 *     is the "SIMPLIFICATION" from the task brief made concrete in
	 *     code, not merely asserted in a comment.
	 */
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
	 * (7) "Invalidate the cache... and read resp_error_code": as with the
	 * flush on the way in, there is no cache to invalidate here -- the
	 * carve-out is non-cacheable, so this barrier plus a direct read is the
	 * whole of step 7 on this memory. The barrier orders the RX-doorbell
	 * read above against the struct read the caller does next.
	 */
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

		ret = se_transport_transact(req, tx, rx);
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

	ret = se_transport_transact(req, tx, rx);
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
	 * log labels, per this file's INFERENCE that SET_VTOR reuses the
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

	ret = se_transport_transact(req, tx, rx);
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
	 * HYPOTHESIS, UNVERIFIED ON SILICON -- see this function's doc comment
	 * in include/alif_se_boot.h: SET_VTOR before BOOT_CPU, derived from
	 * the service enum ordering and the VTOR==0 bench lockup, not from a
	 * confirmed-working sequence. SERVICE_BOOT_RELEASE_CPU (502) is an
	 * untried alternative to the BOOT_CPU step below.
	 */
	ret = alif_se_set_vtor(cpu_id, entry_addr);
	if (ret != 0) {
		LOG_ERR("alif_se_start_cpu(cpu_id=%u): SET_VTOR step failed (%d) -- BOOT_CPU step "
			"NOT attempted",
			cpu_id, ret);
		return ret;
	}
	LOG_INF("alif_se_start_cpu(cpu_id=%u): SET_VTOR step succeeded", cpu_id);

	ret = alif_se_boot_cpu(cpu_id, entry_addr);
	if (ret != 0) {
		LOG_ERR("alif_se_start_cpu(cpu_id=%u): BOOT_CPU step failed (%d)", cpu_id, ret);
		return ret;
	}
	LOG_INF("alif_se_start_cpu(cpu_id=%u): BOOT_CPU step succeeded", cpu_id);

	return 0;
}
