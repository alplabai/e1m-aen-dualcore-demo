# alif_se_boot -- out-of-tree Zephyr module

A standalone Zephyr module providing a client for the Alif Secure Enclave's
BOOT_CPU service (`service_id` 501), used on Alif E8 (Ensemble) silicon to
release a secondary RTSS core -- concretely, the Alp Lab E1M-AEN dual-core
demo's HOST app (`apps/dualcore_host`) releasing its REMOTE peer
(`apps/dualcore_remote`). On the E1M-AEN801 bench unit that is RTSS-HE
releasing RTSS-HP -- see `docs/BENCH-DUALCORE.md` section 0.

This module has **no dependency on alp-sdk** and does not vendor or link the
Alif `se_services` HAL library -- see "Authored from the protocol" below.

## UNVERIFIED ON SILICON

**This has never been run on hardware.** Every register offset, the request
struct layout, and the MHU transport sequence below were transcribed from a
reading of the Alif Secure-Enclave service protocol (as described in a task
brief drawing on the vendor implementation's documented *behaviour*), not
exercised on an actual E1M-AEN board. Treat everything in this module as
"should be correct per the spec as transcribed" -- not "bench-proven" (unlike
the sibling `modules/alif-mhuv2`, which the E1M-AEN801 bench DID validate for
its own, unrelated, doorbell path).

A successful `alif_se_boot_cpu()` call additionally requires that the target
core's image is ALREADY resident at its entry address (e.g. `0x58000000` for
RTSS-HE) -- normally placed there by the Secure Enclave Services (SES) while
processing a two-entry ATOC (Application TOC) whose HE entry is flagged
`["load"]`. This module does not place any code there itself. Releasing a
core whose entry address holds no valid image just starts that core executing
garbage.

## Authored from the protocol, not from Alif's source

**This module was authored from a transcription of the SE service protocol,
not by copying, adapting, or lifting code from Alif's `se_services`
sources.** That distinction matters concretely here: three of the Alif
`se_services` headers this repo's authors read for background carry a
contradictory license header -- `SPDX-License-Identifier: Apache-2.0` at the
top, followed by body text asserting an "All Rights Reserved / Alif
Semiconductor Software License Agreement". Authoring from the protocol
(request/response layout, transport sequence, register facts) rather than
from those files sidesteps that contradiction entirely: nothing in this
module's source, comments, or types originates in Alif's text.

This mirrors how `modules/alif-mhuv2` was written against the ARM MHUv2
register spec (DDI 0515) rather than any vendor driver.

## What it provides

| Piece | Path |
|---|---|
| Kconfig symbol | `CONFIG_ALIF_SE_BOOT` |
| DT compatible (client node) | `alplab,e8-se-boot` |
| DT compatible (frame node) | `alplab,e8-se-mhu-frame` |
| Public header | `include/alif_se_boot.h` |
| Client source | `src/alif_se_boot.c` |
| DT bindings | `dts/bindings/misc/alplab,e8-se-boot.yaml`, `dts/bindings/misc/alplab,e8-se-mhu-frame.yaml` |

Public API is seven functions:

```c
int alif_se_process_toc_entry(const char *image_id);
int alif_se_boot_cpu(uint32_t cpu_id, uint32_t entry_addr);
int alif_se_set_vtor(uint32_t cpu_id, uint32_t vtor_addr);
int alif_se_reset_cpu(uint32_t cpu_id);
int alif_se_release_cpu(uint32_t cpu_id);
int alif_se_ping(void);
int alif_se_start_cpu(uint32_t cpu_id, uint32_t entry_addr);
```

- `alif_se_process_toc_entry()` -- SE service_id 500 (PROCESS_TOC_ENTRY).
  Takes an ATOC entry NAME (`image_id`, e.g. `"ALP-HP"`), not a cpu_id -- a
  DIFFERENT 20-byte wire struct from `alif_se_boot_cpu()`'s
  (`process_toc_entry_svc_t` per the DFP: header + 8-byte `send_entry_id` +
  `resp_error_code`, no cpu_id/address fields at all). Per the DFP, the named
  entry must already be in a **DEFERRED** state (not auto-processed by SES at
  cold boot); this call un-defers it, and SES then does whatever that entry's
  own flags call for (load/verify/boot) AT CALL TIME instead of at cold boot.
  This is the SES-driven alternative to this repo's proven `alif_se_boot_cpu()`
  path against a plain `["load"]`-flagged entry (which the SES table reports
  `uLV` -- Loaded+Verified, not Booted). The DFP source
  (`se_services/templates/services_test.c`) confirms the on-wire flag bit
  (`TOC_IMAGE_DEFERRED = 0x100`) and its SES table letter (`D`, at legend
  position `FLAG_STRING_DEFERRED`) -- **what remains TBD is the ATOC-BUILDER
  JSON field that sets that bit**: every sample ATOC config under
  `alif-setools/app-release-exec-linux/build/config` (every `.json` file
  there) uses only
  `load`/`boot`/`compressed`/`encrypt`, never `deferred`, and `app-gen-toc`
  itself is a PyInstaller binary whose flag vocabulary was not extractable
  from what was in reach (`strings -a` found no "defer" substring in it at
  all). See `alif_se_process_toc_entry()`'s doc comment in
  `include/alif_se_boot.h` for the full account -- do not guess this field
  name against real hardware.
- `alif_se_boot_cpu()` -- SE service_id 501 (BOOT_CPU) only. Does **not**
  transfer any vector table base into the target core's own VTOR register --
  see `alif_se_start_cpu()` below for the sequence that does.
- `alif_se_set_vtor()` -- SE service_id 505 (SET_VTOR). CONFIRMED wire
  struct, per Alif's DFP se_services headers: SET_VTOR genuinely reuses
  BOOT_CPU's 20-byte request/response wire struct, varying only
  `header.service_id` (an earlier revision of this module could only infer
  that reuse from the vendor's declared wrapper signature,
  `SERVICES_boot_set_vtor(services_handle, cpu_id, address, error_code)`;
  the DFP's own struct definitions confirm it directly now). `vtor_addr` is
  a Cortex-M vector table BASE address, not a jump target: on release, the
  core fetches its initial SP from `[vtor_addr]` and its initial PC from
  `[vtor_addr + 4]`. SET_VTOR only ever writes a GLOBAL SE-side VTOR
  register -- `alif_se_reset_cpu()` below is what transfers it into the
  core's own internal VTOR.
- `alif_se_reset_cpu()` -- SE service_id 503 (RESET_CPU). Uses a DIFFERENT,
  16-byte wire struct (`control_cpu_svc_t` per the DFP: header plus
  `send_cpu_id` plus `resp_error_code`, no address field). Per Alif's own
  documented behaviour of `SERVICES_boot_reset_cpu()`: for an M55 core, this
  also transfers whatever value a prior `alif_se_set_vtor()` call wrote into
  the SE's Global VTOR register into that core's own internal VTOR register
  -- the step a `alif_se_boot_cpu()`-only release skips.
- `alif_se_release_cpu()` -- SE service_id 502 (RELEASE_CPU). Same 16-byte
  wire struct as `alif_se_reset_cpu()`. This is the step that actually
  starts the core running.
- `alif_se_ping()` -- the readiness heartbeat (service_id 0) alone, with no
  other request built or sent. Added so a bench operator can probe "is the
  SE awake" without the side effect of releasing or reconfiguring a core --
  `alif_se_boot_cpu()` used to couple heartbeat and BOOT_CPU inseparably,
  which once cost a bench run an unintended HP release during what was meant
  to be a pure reachability probe.
- `alif_se_start_cpu()` -- the correct sequence to start a core at a
  specific vector table base: `alif_se_set_vtor()`, then
  `alif_se_reset_cpu()`, then `alif_se_release_cpu()`, in that order,
  stopping at (and returning) the first failure. This ordering is not a
  guess: it is how Alif's DFP documents these three services' side effects
  (`services_host_boot.c`'s notes on `SERVICES_boot_set_vtor()` and
  `SERVICES_boot_reset_cpu()`). An earlier revision of this function instead
  called `alif_se_set_vtor()` then `alif_se_boot_cpu()`, on a **HYPOTHESIS**
  derived only from the SE service enum's ordering -- that guess is what a
  bench run caught: a released core came up with `VTOR == 0x00000000`,
  fetched its initial SP/PC from its own local address 0 (uninitialized
  garbage), and locked up (`CFSR == 0x00000001` IACCVIOL,
  `HFSR == 0x40000000` FORCED), because BOOT_CPU never transfers the global
  VTOR SET_VTOR staged -- only RESET_CPU does that. **UNVERIFIED ON SILICON
  in this exact three-call form** -- it rests on Alif's own documented
  service behaviour rather than the earlier enum-ordering guess, but this
  specific sequence has not itself been exercised on E1M-AEN801 hardware.

Return-code contract (documented in full in `include/alif_se_boot.h`, shared
by every function that talks to the SE):

- `0` -- SE reports BOOT_CPU succeeded.
- `< 0` (a negative `errno`) -- a LOCAL MHU transport failure on THIS core's
  side of the link (`-ETIMEDOUT` / `-EBUSY` / `-ENOTCONN`, see below). No
  SE-side outcome is known.
- `> 0` -- an SE-REPORTED service error (`resp_error_code`, or
  `service_header.error_code` if that is the only nonzero field). Both
  fields are always logged individually, so neither is silently dropped even
  though only one number can be the return value. Clamped to `INT_MAX` (and
  logged as such) in the vanishingly unlikely case the raw `uint32_t` value
  would otherwise overflow a positive `int` -- so it can never collide with
  the negative-`errno` LOCAL-failure class above.

**Every call sends a readiness heartbeat first.** Before touching
PROCESS_TOC_ENTRY, BOOT_CPU, SET_VTOR, RESET_CPU, or RELEASE_CPU at all,
`alif_se_process_toc_entry()`, `alif_se_boot_cpu()`, `alif_se_set_vtor()`,
`alif_se_reset_cpu()`, and `alif_se_release_cpu()` each
send `service_id 0` (`SERVICE_MAINTENANCE_HEARTBEAT_ID` ==
`SERVICE_MAINTENANCE_START`) and require a reply, retried up to 100 times --
mirroring the vendor's own gate ("ensures the Secure Enclave is awake and
synchronized, ready to process service requests"), which this client
previously skipped. If the SE never answers the heartbeat at all, the
function returns `-ENOTCONN` *without ever sending its own boot-domain
request* -- deliberately distinct from `-ETIMEDOUT`/`-EBUSY` so a bench
operator can tell "the SE never woke" apart from "the SE woke but
refused/timed out on the request itself". `alif_se_ping()` exposes this
exact same heartbeat path standalone, with no boot-domain request ever
built, for probing SE reachability alone.

**Known permanent-`-EBUSY` gap:** after a send-ack timeout specifically
(the SE never latches the pointer written into `CH0_SET`), the TX channel
keeps its bits set. Only the SE (the receiver) can clear its own channel's
STAT register -- this core cannot clear a send it made. Every later call
into this module then returns `-EBUSY` immediately, with no in-band recovery
available from this core alone. Acceptable for a one-shot boot call in this
demo; there is no retry path that helps after that specific failure without
an SE-side reset or a power cycle.

**Not reentrant across cores, is reentrant across local threads:** the
request buffer, both MHU frames, and the transport itself are global,
core-local state, serialized with a `k_mutex` (`se_boot_lock`) so concurrent
callers on THIS core queue safely rather than corrupting each other's
in-flight request. This says nothing about the OTHER core touching the same
hardware concurrently -- only the HOST app (`apps/dualcore_host`, on
whichever cluster it is built for) ever calls this module.

## Design decision: (b), a dedicated transport, not an extension of `alif_mhuv2`

The SE BOOT_CPU request carries a 32-bit payload (the request struct's
global address) over its MHU send frame. The sibling `modules/alif-mhuv2`
MBOX driver is deliberately doorbell-only: its `send()` rejects any non-NULL
`mbox_msg`, and its `mtu_get()` always returns 0.

Two ways to close that gap were considered:

- **(a)** Extend `alif_mhuv2` so channel-window 0 can also carry a 32-bit
  payload for these SE frames.
- **(b)** (chosen) Write a small, self-contained SE transport in THIS module
  that touches the SE-service MHUv2 frame's registers directly, leaving
  `alif_mhuv2` untouched.

**(b) was chosen** because:

1. `alif_mhuv2` is bench-validated on E1M-AEN801 for the RPMsg vring-doorbell
   path. Changing its `send()`/`set_enabled()` contract to grow a second,
   payload-carrying mode is a correctness risk on a proven component, for the
   benefit of exactly one caller.
2. The SE-service frame pair (`mhu@40040000` rx / `mhu@40050000` tx) is a
   PHYSICALLY DIFFERENT instance of the MHUv2 IP from the doorbell pair
   (`mhu@400a0000` / `mhu@400b0000`) -- they never share a device instance,
   so there is no *code* to reuse by routing through the MBOX class API here,
   only register-offset *values* (channel-window SET/STAT/CLEAR,
   ACCESS_REQUEST/ACCESS_READY), which are plain hardware facts and cost
   nothing to re-declare under this module's own names (see
   `src/alif_se_boot.c`'s top-of-file constants).
3. Keeping this transport self-contained means a bug or future change here
   can never regress `alif_mhuv2`'s proven behaviour, and this module can be
   dropped from a build (`ZEPHYR_EXTRA_MODULES`) with zero effect on the
   RPMsg path.

## Memory layout: a dedicated SRAM0 carve-out, not TCM + address translation

The vendor transport places its request struct in TCM and then calls a
`local_to_global()`-style helper, because the Secure Enclave sees TCM at a
different (global) alias than the local core does -- which is also why the
vendor board overlays re-`compatible` the `itcm`/`dtcm` nodes to expose a
`global_base`.

This module avoids all of that: the request struct lives in a small carve-out
of SRAM0, which is ALREADY globally addressable at the same address from
every bus master. The board overlay that instantiates this module's DT node
must ALSO add a `zephyr,memory-region` node for this carve-out, e.g.:

```dts
&soc {
	sram_se_req: memory@2020000 {
		compatible = "zephyr,memory-region", "mmio-sram";
		reg = <0x02020000 0x1000>;
		zephyr,memory-region = "SRAM_SE_REQ";
		zephyr,memory-attr = <(DT_MEM_ARM(ATTR_MPU_RAM_NOCACHE))>;
	};
};
```

`ATTR_MPU_RAM_NOCACHE` is load-bearing, the same way it already is for the
sibling `sram_ipc0` RPMsg vring carve-out in this repo's board `.dts`: it
means the request/response structure needs NO D-cache flush/invalidate at
all (the vendor transport's steps 3 and 7), because there is never a dirty
cache line to push out or a stale one to discard.

### Address arithmetic (no overlap)

- `sram_ipc0` (existing, RPMsg vrings): `[0x02010000, 0x02020000)`
  (`0x02010000 + 0x10000 = 0x02020000`).
- `sram_se_req` (this module, added in the HP app overlays):
  `[0x02020000, 0x02021000)` (`0x02020000 + 0x1000 = 0x02021000`).
- The two carve-outs ABUT at `0x02020000` and do not overlap:
  `sram_ipc0`'s end address equals `sram_se_req`'s start address, and a
  half-open interval comparison (`[a, b)` vs `[b, c)`) has no shared byte.
- Both carve-outs fall inside `sram0` (`[0x02000000, 0x02400000)`, 4 MB, see
  `dts/arm/alif/ensemble/ae402fa0e5597le0.dtsi` /
  `ae822fa0e5597ls0.dtsi` in the Zephyr tree), which is itself disjoint from
  the two GENERIC MPU regions this SoC's Zephyr port defines:
  - `FLASH_0`: `[0x80000000, 0x80580000)` (`CONFIG_FLASH_BASE_ADDRESS` /
    `CONFIG_FLASH_SIZE=5632` KB -- the MRAM region backing `zephyr,flash`).
  - `SRAM_0`: `[0x20000000, 0x20000000 + CONFIG_SRAM_SIZE)`
    (`CONFIG_SRAM_SIZE=1024` KB on the rtss_hp targets -- this is the DTCM
    region backing `zephyr,sram`, NOT `sram0`/SRAM0's own `0x02000000`
    physical range, despite the similar name).

  `sram0`'s physical range (`0x02000000`-range) shares no byte with either
  `0x80000000`-range FLASH_0 or `0x20000000`-range SRAM_0, so a DT-added MPU
  region inside `sram_se_req` cannot land inside a generic region. On this
  silicon (`CONFIG_MPU_REQUIRES_NON_OVERLAPPING_REGIONS=y`), a DT region that
  DID land inside a generic one would multi-match on every access to it --
  a documented self-bricking failure mode this arithmetic avoids.

## How to point Zephyr at this module

```sh
west build -b <board_target> <app_dir> \
    -- \
    -DZEPHYR_EXTRA_MODULES="/path/to/e1m-aen-dualcore-demo/modules/alif-mhuv2;/path/to/e1m-aen-dualcore-demo/modules/alif-se-boot"
```

Both modules are independent and can be listed in either order; this repo's
`scripts/build-all.sh` and `apps/dualcore_host` already wire both in.

## Directory layout

```
modules/alif-se-boot/
+-- zephyr/module.yml               # module manifest (cmake/kconfig/dts_root)
+-- CMakeLists.txt                  # module root -- zephyr_library() (flat, one TU)
+-- Kconfig                         # module root -- CONFIG_ALIF_SE_BOOT
+-- include/
|   +-- alif_se_boot.h              # public API
+-- src/
|   +-- alif_se_boot.c              # the client
+-- dts/bindings/misc/
    +-- alplab,e8-se-boot.yaml           # the logical client node's binding
    +-- alplab,e8-se-mhu-frame.yaml      # one SE-dedicated MHUv2 frame's binding
```

## Attaching the DT node (app overlay, not the shared board `.dts`)

Only `apps/dualcore_host` releases its peer, so both the `sram_se_req`
carve-out and the `se_boot` node live in every
`apps/dualcore_host/boards/*.overlay` (both qualifiers -- see that app's
README for why it builds for both) -- never in the shared
`boards/alp/e1m_aen/*.dts`, and never in `apps/dualcore_remote`.
