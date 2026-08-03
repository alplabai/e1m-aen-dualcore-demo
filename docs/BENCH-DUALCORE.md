# Reproducing the proven E1M-AEN801 dual-core RPMsg runs

Two runs matter here, and they release the peer core two different ways:

- **2026-07-31, the primary / customer-relevant path** -- SES-driven release
  of a **deferred** ATOC entry (Alif Secure Enclave `service_id` 500,
  `SERVICES_boot_process_toc_entry`, wrapped in this repo as
  `alif_se_process_toc_entry()`): **495 consecutive `PING`/`PONG`
  round-trips over 4m11s, no drop, no gap.** This is the mechanism a
  customer carrier would actually ship: it survives a power cycle and needs
  no debugger attached. It is documented as the PRIMARY procedure in
  section 2 below. **The ATOC file this run depends on is not committed to
  this repository -- see section 2.2. Until it is, this result cannot be
  reproduced from a clean clone.**
- **2026-07-30, a bench-only alternative** -- a debugger loads and starts
  the peer core directly, standing in for the SETOOLS/ATOC mechanism: 369
  consecutive `PONG seq=N rtt=32..35 us` lines, no gaps, still running at
  `seq=563` when the session ended. This does **not** survive a power cycle
  and requires a debugger permanently attached; a customer carrier would
  not do this in production. It is documented as the bench-only
  alternative in section 3.

**Read this before assuming the bench shape matches the repo's board-naming
convention.** It does not, exactly -- see step 0.

## 0. The bench shape is the MIRROR of what this repo's OLD `rtss_hp`/`rtss_he`-named apps assumed

At the time of the 2026-07-30 run described below, this repo's apps were
named `dualcore_hp`/`dualcore_he` and were written under the assumption that
the `rtss_hp` cluster is the one with Secure Enclave (SE) access and boots
first. **That assumption did not hold on the bench.** What was actually
observed:

- The resident ATOC (Application Table of Contents) already on the bench
  silicon boots **RTSS-HE** first: `ALP-HE | M55-HE | Boot Addr 0x80010000`.
  RTSS-HE is therefore the **SES-booted core** on this bench, not RTSS-HP.
- **RTSS-HE runs the RPMsg HOST** role and has SE access on this bench: it
  calls `alif_se_boot_cpu(2 /* M55-HP, EXTSYS_0 */, 0x50000000)` to release
  RTSS-HP.
- **RTSS-HP runs the RPMsg REMOTE** role. It is released by the SE call
  above, then loaded by a debugger (NOT by the SE/ATOC) into its own LOCAL
  `0x00000000` through an Access Port (AP) at `APAddr 0x00200000` that only
  appears in the DAP's AP list AFTER the release, then started by register
  surgery (section 3.4 below, for the bench-only debugger-placement flow --
  this describes the 2026-07-30 run specifically; the 2026-07-31 run in
  section 2 released RTSS-HP a different way).

**This has since been fixed.** The apps are now named by RPMsg role, not by
cluster -- `apps/dualcore_host` and `apps/dualcore_remote` -- and each builds
for both the `rtss_hp` and `rtss_he` board qualifiers (see each app's own
README). `apps/dualcore_host`'s `rtss_he` overlay now instantiates the same
`se_boot` devicetree node its `rtss_hp` overlay always did, so `west build
... -b e1m_aen/ae822fa0e5597ls0/rtss_he apps/dualcore_host` builds an image
that is directly flashable/placeable on the SES-booted cluster -- no
hand-written scratch host app needed anymore. Reproducing the run with THIS
repo's images now means:

- Build `apps/dualcore_host` for `e1m_aen/ae822fa0e5597ls0/rtss_he` (its
  Kconfig defaults, `CONFIG_DEMO_RELEASE_PEER_CPU_ID=2` /
  `CONFIG_DEMO_RELEASE_PEER_ENTRY=0x50000000`, already encode the
  bench-proven release parameters -- see `apps/dualcore_host/Kconfig` and
  `apps/dualcore_host/src/main.c`'s top-of-file comment).
- Build `apps/dualcore_remote` for `e1m_aen/ae822fa0e5597ls0/rtss_hp`.
- What IS reproducible today from this repo, unmodified: both `west build`
  commands in section 3.1 now succeed and produce correctly-sized images --
  `apps/dualcore_host` built for `rtss_he` links with `CONFIG_SRAM_SIZE=256`
  (matching RTSS-HE's real 256 KB DTCM) and its `se_boot` node is present,
  where previously only a hand-written scratch app not committed to this
  repo could do so. What is NOT independently re-confirmed by this fix: an
  actual silicon run of THIS lineage (new build -> debugger placement per
  sections 3.2-3.4) has not been repeated on the bench since the rename;
  the 369-PONG count above was measured against the pre-rename scratch-app
  build. The two are expected to be functionally identical (same Kconfig
  defaults, same overlay content, same CONFIG_SRAM_SIZE), but that is an
  inference, not a fresh bench measurement -- see section 4 for what
  remains open, and section 3 for what is bench-specific either way.

## 1. Which core boots first, and why

The bench silicon already carries a resident ATOC that boots **M55-HE**
(RTSS-HE) at MRAM-XIP address `0x80010000` on power-up/reset -- this is a
fact about the SES (Secure Enclave Services) configuration already on this
specific bench unit, not something this repo's build produces. Every step
below assumes that resident ATOC is already in place and unchanged.

## 2. The deferred-ATOC path (primary -- survives a power cycle, no debugger required)

### 2.1 What is proven and measured

Measured 2026-07-31: **495 consecutive PING/PONG round-trips over 4m11s, no
drop, no gap.** Role mapping is the same as sections 0/1: HOST on RTSS-HE
(SES-booted, SE access), REMOTE on RTSS-HP (the peer this run releases).

Only the REMOTE (peer) console was captured for this run. From cold boot:

```
*** Booting Zephyr OS build v4.4.0 ***
<inf> dualcore_remote: board target: e1m_aen/ae822fa0e5597ls0/rtss_hp
<inf> dualcore_remote: ipc0 open -- host is alive
<inf> dualcore_remote: endpoint bound; waiting for PING
<inf> dualcore_remote: PING seq=495 received; echoing PONG
```

No HOST-side (RTSS-HE) transcript is part of the facts established for this
document -- whether one was captured at all is **TBD**.

Corroborated by the peer beacon in shared SRAM0 (see
`apps/dualcore_remote/src/main.c`'s `BEACON_ADDR_*` map):

| Address | Field | Observed value |
|---|---|---|
| `0x02000100` | `BEACON_ADDR_MAGIC` | `0xC3C33C3C` |
| `0x02000104` | `BEACON_ADDR_CORE_ID` | `0x00000002` (M55-HP) |
| `0x0200010C` | `BEACON_ADDR_PHASE` | `0x00000003` (endpoint bound) |
| `0x02000110` | `BEACON_ADDR_RC_OPEN` | `0x00000000` |
| `0x02000114` | `BEACON_ADDR_RC_REGISTER` | `0x00000000` |
| `0x02000108` | `BEACON_ADDR_HEARTBEAT` | advancing `0x117` -> `0x172` -> `0x186` -> `0x19A` over the session |

The release mechanism was Secure-Enclave `service_id` 500
(`SERVICES_boot_process_toc_entry`, wrapped as `alif_se_process_toc_entry()`
in `modules/alif-se-boot`), called against a **deferred** ATOC entry -- see
2.2. The peer ATOC entry's on-the-wire `flags` word was observed going
`0x00000022` -> `0x00000122` (`TOC_IMAGE_DEFERRED = 0x100`), and the SES
boot table printed `D` in the flag column for it. On this run, the runtime
`alif_se_process_toc_entry()` call performed load, verify, and release
together -- **no separate `alif_se_boot_cpu()` (`BOOT_CPU`, `service_id`
501) call was required.** (Section 2.5 below records an earlier,
contradicting bench observation on this exact point -- read it before
assuming this is settled.)

### 2.2 The ATOC this run depends on -- NOT committed to this repository

**The entire 495-PING/PONG result depends on a two-entry ATOC (Application
Table of Contents) JSON, built with Alif SETOOLS' `app-gen-toc`, that is
not committed anywhere in this tree.** Until it is, this result CANNOT be
reproduced from a clean clone -- not by another engineer, not by a
customer. **The authoritative ATOC JSON file must be committed to close
this gap.**

What is established about its required shape, verbatim, and nothing more --
every field not listed below is **TBD**; do not invent `app-gen-toc`'s JSON
schema keys or values to fill a gap, a wrong ATOC costs a bench cycle:

- Peer (REMOTE) entry, name `"ALP-HP"`: cpu_id M55_HP, loadAddress
  `0x50000000`, `flags: ["load", "boot", "deferred"]` -- this is the
  working shape that produced the 495-PING/PONG run in 2.1. `"deferred"`
  is a valid MEMBER of the entry's `flags` ARRAY, alongside `"load"` /
  `"boot"` -- a sibling `"deferred": true` KEY is rejected by the ATOC
  builder. It sets `TOC_IMAGE_DEFERRED = 0x100` in the entry's on-the-wire
  flags word.
- Host entry, name `"ALP-HE"`: cpu_id M55-HE, Boot Addr `0x80010000`. This
  entry's own `flags` array value is **TBD** -- not established by this
  project's bench sessions. (The SES boot table shows it boots at cold
  boot with no operator action, so it plausibly does not carry
  `"deferred"`, but that is an inference from behaviour, not a confirmed
  field value -- do not encode it as fact.)
- Every other field of the ATOC JSON schema -- exact key names (e.g.
  whether the peer's load address key is literally `loadAddress`), image
  binary file references, any signing/certificate fields, ATOC-level
  metadata (version, entry count, ...) -- is **TBD**. Consult Alif
  SETOOLS' `app-gen-toc` documentation or a working example ATOC, not this
  document, to fill them in.

### 2.3 Build command that actually produces the proven configuration

**A default build of this repo does NOT produce the 495-PING/PONG
configuration.** In `apps/dualcore_host/Kconfig`, both
`CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY` and `CONFIG_DEMO_RELEASE_TOC_THEN_BOOT`
default to `n`, so a default build ships the `#else` fallback
(`alif_se_start_cpu()`, `service_id` 501 `BOOT_CPU` against a plain
`["load"]`-flagged entry) -- not the deferred-TOC release the 495-PING run
used. `scripts/build-all.sh` passes neither option, so it also ships the
fallback. Building this repo unmodified does NOT reproduce the proven run.

To build the HOST image the way the 495-PING/PONG run actually used it, add
`-DCONFIG_DEMO_RELEASE_VIA_TOC_ENTRY=y` to the same MRAM-XIP build command
`apps/dualcore_host/README.md` documents as the bench-proven default (this
is a normal MRAM/ATOC boot, not the ITCM/debugger-placement build in
section 3):

```sh
west build -p auto \
  -b e1m_aen/ae822fa0e5597ls0/rtss_he \
  -d build/ae822fa0e5597ls0/rtss_he \
  <repo>/apps/dualcore_host \
  -- \
  -DBOARD_ROOT=<repo> \
  "-DZEPHYR_EXTRA_MODULES=<repo>/modules/alif-mhuv2;<repo>/modules/alif-se-boot" \
  -DCONFIG_DEMO_RELEASE_VIA_TOC_ENTRY=y
```

REMOTE builds exactly as `apps/dualcore_remote/README.md` documents (no
Kconfig change needed on that side -- the deferred-release mechanism is
entirely a HOST-side/SE concern):

```sh
west build -p auto \
  -b e1m_aen/ae822fa0e5597ls0/rtss_hp \
  -d build/ae822fa0e5597ls0/rtss_hp \
  <repo>/apps/dualcore_remote \
  -- \
  -DBOARD_ROOT=<repo> \
  -DZEPHYR_EXTRA_MODULES=<repo>/modules/alif-mhuv2
```

For a bench-readable record of which SE step, if any, failed, pair the
Kconfig option above with the breadcrumb overlay:
`-DCONFIG_DEMO_RELEASE_VIA_TOC_ENTRY=y -DEXTRA_CONF_FILE=breadcrumb.conf` --
the `PROCESS_TOC_ENTRY` return value lands at global SRAM0 `0x02000040`
(see `apps/dualcore_host/Kconfig`'s `CONFIG_DEMO_EXECUTION_BREADCRUMB` help
text for the full breadcrumb map).

### 2.4 Flashing order

Same ordering rule as any SETOOLS/ATOC build: build both images first, then
write both MRAM slots (`slot0_partition` for `rtss_hp` at `0x000000`, then
`slot0_partition` for `rtss_he` at `0x300000` -- see root `README.md`
section 5), then write the ATOC last -- the ATOC entry references both
slots, so it must be written only after both slots it points to already
exist in MRAM. For the deferred-entry shape in 2.2, this ordering still
applies to the peer's `["load", "boot", "deferred"]` entry: its image must
already be written to its MRAM slot before the ATOC that references it is
written, exactly as for a non-deferred entry -- `"deferred"` changes WHEN
the SES processes the entry (at the runtime `alif_se_process_toc_entry()`
call instead of at cold boot), not whether the image needs to already be in
MRAM first.

### 2.5 Open disagreement: does the deferred entry alone release the peer?

Two places in this tree currently disagree on this exact point, and this
document is not resolving it -- both observations are recorded below,
verbatim, pending a fresh bench run:

- `apps/dualcore_host/Kconfig`'s help text for
  `CONFIG_DEMO_RELEASE_TOC_THEN_BOOT` states: on E1M-AEN801 silicon
  (2026-07-31), un-deferring the `ALP-HP` ATOC entry via
  `alif_se_process_toc_entry()` (SE `service_id` 500, `PROCESS_TOC_ENTRY`)
  bench-confirmed it MATERIALIZES the peer's image at
  `CONFIG_DEMO_RELEASE_PEER_ENTRY` (`0x50000000`) -- SRAM there went from
  uninitialized to the staged binary's first 16 words, all matching, in
  one boot -- **but the peer core still did not start executing** (its
  beacon at `0x02000100` never reached `0xC3C33C3C`).
- Section 2.1 above records the opposite: with the peer entry flagged
  `["load", "boot", "deferred"]`, the `alif_se_process_toc_entry()` call
  alone produced 495 consecutive PING/PONG round-trips, and the beacon DID
  reach `0xC3C33C3C` (core id `0x00000002`, phase `0x00000003`), with no
  separate `alif_se_boot_cpu()` (`BOOT_CPU`, `service_id` 501) call in the
  path.

This is unresolved pending a fresh bench run. Do not assume either
observation supersedes the other. `CONFIG_DEMO_RELEASE_TOC_THEN_BOOT`
remains in the tree, `default n`, as the fallback strategy (un-defer, then
an explicit `BOOT_CPU` call) in case a future run needs it.

### 2.6 Known false root cause -- read this before spending a day on it

The in-flight PR's own commit argues that D-cache maintenance had to be
restored on the SE request buffer at SRAM0 `0x02020000` because the
devicetree tag `zephyr,memory-attr =
<(DT_MEM_ARM(ATTR_MPU_RAM_NOCACHE))>` does not actually program an MPU
region. **That has been disproved against the real toolchain:**

- Zephyr v4.4.0 `arch/arm/core/mpu/arm_mpu.c:561-568` calls
  `mpu_configure_regions_from_dt()` under `#ifdef CONFIG_MEM_ATTR`.
- That function is `static` -- which is why it has no ELF symbol; its
  absence was misread as "not linked."
- `CONFIG_MEM_ATTR=y` and `CONFIG_ARM_MPU=y` are both set in the built
  config.
- The linked ELF's `mem_attr_region` table contains the entry
  `addr=0x02020000 size=0x00001000 attr=0x00200000`, i.e.
  `ATTR_MPU_RAM_NOCACHE`.

The region IS non-cacheable, so the cache maintenance added alongside it is
a harmless no-op. **Consequence: the true root cause of the `-116`
SE-transport failure this cache maintenance was meant to fix is NOT
established, and that failure may recur.** If `-116` (`-ETIMEDOUT`) from
the SE transport shows up again, do not re-open the cache-attribute
theory -- it is closed. Look elsewhere.

The one link the ELF alone could not close was hardware: the `0x02020000`
entry lands at MPU region index 4 (after the two static regions and the
`0x1a000000` and `0x02010000` DT regions), so it needs `MPU_TYPE.DREGION`
to be at least 5. The M55 MPU is configurable at 0/4/8/12/16 regions; at
DREGION=4 that entry alone would fail to allocate and fall back to the
ARMv8-M default map -- Code region `0x00000000`-`0x1FFFFFFF`, Normal
write-through **cacheable** -- flipping the whole conclusion.

**Measured on this bench (2026-08-01, read-only SWD, RTSS-HE core at
AP `0x300000`, `DPIDR 0x4c013477`):**

```
MPU_TYPE  0xE000ED90 = 0x00001000    DREGION = 16
MPU_CTRL  0xE000ED94 = 0x00000005    ENABLE=1, HFNMIENA=0, PRIVDEFENA=1
MPU_MAIR0 0xE000EDC0 = 0x0044FFAA    Attr2 = 0x44 = NORMAL_OUTER_INNER_NON_CACHEABLE
MPU_MAIR1 0xE000EDC4 = 0x00000000
CPUID     0xE000ED00 = 0x411FD220    Cortex-M55 r1p0
```

`DREGION = 16` is the maximum of the configurable options, so region index
4 has ample room and the feared failure mode does not exist on this
silicon. `DREGION` is a pure hardware property, valid regardless of which
image is resident. Attr2 = `0x44` also confirms Zephyr's ARMv8-M attribute
table is the one loaded.

**What that reading does NOT establish.** The image resident in MRAM slot0
at the time was not one of ours -- `VTOR = 0x80010000`, reset handler
`0x80013225` (the canonical `person_detect` slot0 is `0x80011F15`) -- and
it programs only three regions: `0x80000000` (MRAM), `0x20000000` (DTCM),
`0x1A000000` (device). It declares no `sram_se_req`, so indices 3-15 read
`RBAR=0x00000000 RLAR=0x00000000` with `EN=0`. Note what follows for that
image, since `PRIVDEFENA=1`: `0x02020000` is unmapped there and DOES fall
through to the cacheable default map. That is what an image WITHOUT the
devicetree carve-out looks like -- it is not what our build does.

To confirm our own build programs region 4 as expected, flash it and
re-run exactly these reads, then check for `RBAR` base `0x02020000` with
`RLAR` limit `0x02020FFF` and `RLAR.AttrIndx = 2`. That step has not been
performed.

Reading these registers needs no flashing and no reset. Attach read-only
over SWD, halt, then read `MPU_TYPE` at `0xE000ED90`, `MPU_CTRL` at
`0xE000ED94`, `MPU_MAIR0`/`MPU_MAIR1` at `0xE000EDC0`/`0xE000EDC4`, then
for each region index write the index to `MPU_RNR` at `0xE000ED98` and
read `MPU_RBAR` at `0xE000ED9C` and `MPU_RLAR` at `0xE000EDA0`. Note that
`0xE000ED90` is `MPU_TYPE` and is read-only -- `MPU_RNR` is `0xE000ED98`;
confusing the two is easy and would be the session's only write. Use the
RNR-then-RBAR/RLAR sequence, not the `MPU_RBAR_A1/A2/A3` aliases. Read
every value twice in separate sessions and discard the run if they differ.

## 3. Bench-only alternative: debugger-placement path (2026-07-30 run; does not survive a power cycle)

Everything in this section exists because, on 2026-07-30, this bench had no
SETOOLS/ATOC entry for the RTSS-HP image and a debugger stood in for that
mechanism session by session. A customer carrier would NOT do this in
production: it does not survive a power cycle and requires a debugger
permanently attached. It remains documented in full below because the
hard-won bench details (traps, timings, register values) are worth more
than the prose around them, and because this is still a legitimate
fallback flow when no ATOC is available.

### 3.1 Build both images, ITCM-retargeted

Both images are built to run out of ITCM (not MRAM-XIP) for this procedure,
so a debugger can place them directly at each core's local `0x00000000`
without going through the MRAM/ATOC path at all.

`-DBOARD_ROOT` must be an ABSOLUTE path -- `-DBOARD_ROOT=.` fails with
`Invalid BOARD; see above.` because CMake evaluates it relative to the build
directory, not the shell's current directory, so a bare `.` does not resolve
to this repo's root the way it looks like it should.

`-DDTC_OVERLAY_FILE=itcm.overlay` ALONE **replaces** the board's
auto-picked overlay list instead of adding to it, silently dropping the
app's own board overlay (the one that instantiates `ipc0`, `mhu_tx`/`mhu_rx`,
and -- for the host build -- `se_boot`). The build still configures, but
fails later with `'__device_dts_ord_DT_N_NODELABEL_ipc0_ORD' undeclared`.
Pass BOTH files, semicolon-separated, so the auto-picked overlay is still
applied alongside the ITCM retarget:

```sh
export ZEPHYR_BASE="$HOME/zephyrproject/zephyr"   # adjust to your west workspace
REPO="$(pwd)"   # this repo's absolute path -- BOARD_ROOT below must be absolute

west build -p auto \
  -b e1m_aen/ae822fa0e5597ls0/rtss_he \
  -d build/ae822fa0e5597ls0/rtss_he-itcm \
  apps/dualcore_host \
  -- \
  "-DBOARD_ROOT=${REPO}" \
  "-DZEPHYR_EXTRA_MODULES=${REPO}/modules/alif-mhuv2;${REPO}/modules/alif-se-boot" \
  "-DDTC_OVERLAY_FILE=boards/e1m_aen_ae822fa0e5597ls0_rtss_he.overlay;itcm.overlay"

west build -p auto \
  -b e1m_aen/ae822fa0e5597ls0/rtss_hp \
  -d build/ae822fa0e5597ls0/rtss_hp-itcm \
  apps/dualcore_remote \
  -- \
  "-DBOARD_ROOT=${REPO}" \
  "-DZEPHYR_EXTRA_MODULES=${REPO}/modules/alif-mhuv2" \
  "-DDTC_OVERLAY_FILE=boards/e1m_aen_ae822fa0e5597ls0_rtss_hp.overlay;itcm.overlay"
```

`itcm.overlay` (identical for both apps, and now committed in-tree at
`apps/dualcore_host/itcm.overlay` / `apps/dualcore_remote/itcm.overlay` --
earlier drafts of this document only showed its contents without the file
actually existing in the repo):

```dts
/ {
	chosen {
		zephyr,flash = &itcm;
		/delete-property/ zephyr,code-partition;
	};
};
```

**Use the path-ref `&itcm` form, not the `<&itcm>` phandle-array form.** The
phandle-array form (`zephyr,flash = <&itcm>;`) parses but gives
`FLASH_SIZE=0` in the generated devicetree, which then overflows the linker
script (the image has nowhere to link). The path-ref form above is the one
that actually works.

### 3.2 The HE Flow C run (debugger loads and starts the resident-boot core)

This step is for the cluster the resident ATOC boots on its own (RTSS-HE on
this bench) -- "Flow C" here just names the load-then-settle-then-go sequence
used, distinct from Flow D (MRAM-XIP, mentioned in the board `.dts` comments)
and from ordinary `west flash`.

1. `loadbin build/ae822fa0e5597ls0/rtss_he-itcm/zephyr/zephyr.bin 0x0` --
   load the ITCM-retargeted HOST image (built for `rtss_he` per section 3.1)
   to its own local address 0.
2. `SetPC <entry>` -- set PC to the image's reset handler, read from the
   vector table / `.bin`'s word[1] WITH BIT 0 CLEARED (e.g. `0x265C`, not
   `0x265D`) -- the same reset-vector convention used in section 3.4 below.
3. **`Sleep 20000`** -- a 20-second settle BEFORE `go`. **This is required,
   not a safety margin.** Measured across six runs: with no settle, console
   output is zero every time. The cause was not root-caused further within
   this bench session; treat the 20 s as load-bearing until it is.
4. `go` -- release the core to run.

### 3.3 The OpenOCD config that reaches the released HP core

RTSS-HP does not exist as a debuggable target until AFTER the SE release in
section 3.2 of the app's own boot sequence (`alif_se_boot_cpu(2,
0x50000000)`, called from the running HE image) -- its Access Port only
appears in the DAP's AP list post-release, at `APAddr 0x00200000`. This
OpenOCD config reaches it once that has happened:

```
adapter driver jlink
adapter usb location 3-4.4.3
transport select swd
adapter speed 1000
reset_config none separate
gdb_port disabled
tcl_port disabled
telnet_port disabled
swd newdap chip cpu -enable
dap create chip.dap -chain-position chip.cpu -adiv6
target create chip.hp cortex_m -dap chip.dap -ap-num 0x00200000
init
```

**`cortex_m` (NOT `mem_ap`) is required** for register access (`msp`, `sp`,
`pc`, `xPSR`, `CFSR`, `HFSR`) in section 3.4 below -- a plain `mem_ap`
target only gives raw memory access, not the CPU register set. `init` also
auto-clears the lockup state the core may be in immediately after release;
no separate unlock step is needed.

### 3.4 HP placement + register surgery (the debugger IS the boot mechanism here)

**The ordering warning, in bold, first: the SE release in section 3.2's
`alif_se_boot_cpu()` call WIPES the target core's TCM. An image written to
RTSS-HP's local `0x00000000` BEFORE that release is destroyed by it. The
image MUST be written AFTER `BOOT_CPU` (`service_id` 501) has already run,
never before.** This is why the HP image cannot simply be pre-loaded and
left resident the way the HE image is in section 3.2 -- the release itself
erases whatever was there.

Once the peer's `alif_se_boot_cpu(2, 0x50000000)` call (from the running
HOST image, `apps/dualcore_host`'s Kconfig-default release target) has
returned success and `chip.hp` is reachable per section 3.3:

1. `load_image build/ae822fa0e5597ls0/rtss_hp-itcm/zephyr/zephyr.bin
   0x00000000` -- write the ITCM-retargeted REMOTE image (built for
   `rtss_hp` per section 3.1) to the core's own local address 0.
2. Verify the first four words in target memory against the `.bin` file
   byte-for-byte before proceeding -- catches a truncated/failed
   `load_image` before it wastes a debug cycle on a corrupt image.
3. Register surgery, from the loaded image's own first two words (the
   Cortex-M reset convention: word[0] = initial SP, word[1] = initial PC
   with bit 0 set for Thumb):
   - `msp` = word[0]
   - `sp` = word[0]
   - `pc` = word[1] with bit 0 CLEARED (the debugger sets Thumb state via
     `xPSR`, not via a set bit in `pc`)
   - `xPSR` = `0x01000000` (T-bit set, everything else clear)
4. Write-1-to-clear the fault status registers, in case the release left
   the core in a fault state from the earlier VTOR==0 lockup class of
   failure -- observed fault state before clearing: `CFSR = 0x00000101`
   (`IACCVIOL` + `IBUSERR`), `PC = 0xEFFFFFFE`:
   - `CFSR` (`0xE000ED28`) — write back its own current value
   - `HFSR` (`0xE000ED2C`) — write back its own current value
5. **Leave `VTOR` at its reset value, `0x00000000`.** Do NOT set it to the
   image's actual link address. The core fetches SP/PC from its own local
   `0x00000000` on this path regardless of `VTOR` -- see the note in
   `apps/dualcore_host/Kconfig`'s `CONFIG_DEMO_RELEASE_PEER_ENTRY` help text
   on why the SE-reported entry address is decorative on this path.
6. `resume` -- release the core to run from the `pc`/`sp` just written.

### 3.5 Expected output on both sides

HE side (host, `apps/dualcore_host` logic, running from the resident-boot
image placed per section 3.2). This is the only side actually captured on
this bench -- see the note below:

```
=== Alp Lab E1M-AEN dualcore demo -- HOST ===
board target: e1m_aen/ae822fa0e5597ls0/rtss_hp, console: uart@...
SE released peer cpu_id=2, reported entry 0x50000000
endpoint bound; starting PING/PONG
PONG seq=0 rtt=33 us
PONG seq=1 rtt=32 us
...
PONG seq=563 rtt=34 us
```

HP side (remote, `apps/dualcore_remote` logic, running from the image
placed and started by the debugger per section 3.4). Expected content
shown for completeness, but see the note below for why this side's console
could not actually be captured on this bench:

```
=== Alp Lab E1M-AEN dualcore demo -- REMOTE ===
board target: e1m_aen/ae822fa0e5597ls0/rtss_he, console: uart@...
opening ipc0 as RPMsg remote -- BLOCKS here until the ... host signals virtio DRIVER_OK in shared memory
ipc0 open -- host is alive
endpoint bound; waiting for PING
PING seq=0 received; echoing PONG
PING seq=1 received; echoing PONG
...
```

The `board target:` lines above reflect this repo's own board-naming
convention (`rtss_hp`/`rtss_he`), which names which app built the image, not
necessarily which silicon cluster it ran on during this particular bench
session -- see section 0.

**The HP-side (remote) transcript above is NOT actually capturable on this
bench.** The remote app's console is `uart3`, but only UART5 is physically
routed to this bench's `/dev/ttyUSB2`. The HOST-side PONG count (369
consecutive, no gaps) is the confirmation that the remote side was in fact
running and echoing correctly -- every PONG implies a prior successful PING
round trip -- but no direct transcript of the remote's own log lines exists
from this bench session.

On the debug side, confirmation that the release reached the core: HP's
`NVIC_ISER1 = 0x00000800` (IRQ 43, the RPMsg doorbell, enabled) was read back
after `resume`.

### 3.6 Safety notes

- **Verify the SW-DP ID before any write.** This bench's probe must read
  `0x4C013477`. A read of `0x0BE12477` means the adapter is talking to a
  DIFFERENT board's cloned probe, reachable at USB path `3-4.2` on this
  bench's USB topology -- do not proceed with any write against that ID.
  A DP-only check that never touches the target -- just section 3.3's
  OpenOCD config with its `target create chip.hp ...` line removed -- gives
  the SWD DPIDR with no failed AP examination to wade through:

  ```
  adapter driver jlink
  adapter usb location 3-4.4.3
  transport select swd
  adapter speed 1000
  reset_config none separate
  gdb_port disabled
  tcl_port disabled
  telnet_port disabled
  swd newdap chip cpu -enable
  dap create chip.dap -chain-position chip.cpu -adiv6
  init
  ```

  Expect `DPIDR 0x4c013477` and no `Examination failed` lines -- with
  `target create` present, OpenOCD also tries to examine the AP, which
  fails (as expected) before RTSS-HP is released and adds noise to a check
  that is only about the DP.
- **Never issue `SYSRESETREQ` through the HP AP.** The whole point of this
  procedure is that the HP core has no independent reset/boot path once
  released this way; a `SYSRESETREQ` here does not do what it would on a
  normal standalone target and has not been characterized.
- **Hold a labgrid reservation for the entire session.** Sections 3.2-3.5
  above depend on state (the SE release, the AP's post-release appearance,
  the core's fault-register contents) that another user's concurrent
  session on the same bench would silently corrupt.

## 4. Known gaps / not yet closed

- **The ATOC file behind the 2.1 result is not committed to this
  repository** -- see 2.2. A reader cannot reproduce the 495-PING/PONG run
  from a clean clone until it is.
- **The shipped default does not build the proven configuration** -- see
  2.3. Both `CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY` and
  `CONFIG_DEMO_RELEASE_TOC_THEN_BOOT` default `n`, and `scripts/build-all.sh`
  passes neither.
- **Whether the deferred entry alone releases the peer, or a separate
  `BOOT_CPU` call is also required, is an open disagreement** between
  `apps/dualcore_host/Kconfig`'s help text and the 2.1 result -- see 2.5.
  Unresolved pending a fresh bench run.
- **The true root cause of the `-116` SE-transport failure the in-flight
  PR's D-cache-maintenance fix targeted is NOT established** -- that fix's
  own stated rationale (the `zephyr,memory-attr` tag not programming an MPU
  region) has been disproved against the linked ELF's own
  `mem_attr_region` table. See 2.6. The failure may recur with no known
  fix.
- No HOST-side (RTSS-HE) console transcript for the 2.1 run is among the
  facts established for this document -- **TBD** whether one exists.
- The debugger-placement lineage in section 3 has not been repeated on the
  bench since the `dualcore_hp`/`dualcore_he` -> `dualcore_host`/
  `dualcore_remote` rename -- see section 0's final paragraph.
