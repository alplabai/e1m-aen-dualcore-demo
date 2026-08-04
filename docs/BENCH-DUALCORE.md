# Reproducing the proven E1M-AEN801 dual-core RPMsg runs

Three runs matter here. Two release the peer core the same way (a deferred
SETOOLS/ATOC entry); the third is a bench-only alternative:

- **2026-08-04, the reproducibility-closing run** -- this repo's exact
  committed, shipped-default combination (`atoc/e1m-aen801-dualcore.json`,
  the `CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY` default, the corrected MRAM slot
  map), exercised end to end via `scripts/flash-dualcore.sh` on a clean
  clone -- no by-hand ATOC, no by-hand Kconfig override. See section 2.7
  for the full record.
- **2026-07-31, the first deferred-ATOC result** -- SES-driven release
  of a **deferred** ATOC entry (Alif Secure Enclave `service_id` 500,
  `SERVICES_boot_process_toc_entry`, wrapped in this repo as
  `alif_se_process_toc_entry()`): **495 consecutive `PING`/`PONG`
  round-trips over 4m11s, no drop, no gap.** This is the mechanism a
  customer carrier would actually ship: it survives a power cycle and needs
  no debugger attached. It is documented as the PRIMARY procedure in
  section 2 below. **The ATOC file this run's SHAPE depends on is now
  committed to this repository at `atoc/e1m-aen801-dualcore.json` -- see
  section 2.2.** This first run's own ATOC was by-hand and its own Kconfig
  override was by-hand, under the OLD (pre-fix) slot map, not this repo's
  current defaults -- the 2026-08-04 run above is what confirms the
  committed combination itself.
- **2026-07-30, a bench-only alternative** -- a debugger loads and starts
  the peer core directly, standing in for the SETOOLS/ATOC mechanism: 369
  consecutive `PONG seq=N rtt=32..35 us` lines, no gaps, still running at
  `seq=563` when the session ended. **`PONG seq=N` is HOST-side output**
  (see the HOST sample in section 3.5), captured from the pre-rename
  scratch build (before the `dualcore_hp`/`dualcore_he` ->
  `dualcore_host`/`dualcore_remote` rename -- see section 0); this repo,
  unmodified, cannot capture that same side on this bench today, because
  the HOST role's console (`uart3`) is not physically routed there (section
  3.5). This run does **not** survive a power cycle and requires a debugger
  permanently attached; a customer carrier would not do this in production.
  It is documented as the bench-only alternative in section 3, including a
  2026-08-03 re-run of the same procedure that did not reproduce this
  result (section 3.4).

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
- **RTSS-HP runs the RPMsg REMOTE** role. It is targeted by the SE call
  above, then loaded by a debugger (NOT by the SE/ATOC) into its own LOCAL
  `0x00000000` through an Access Port (AP) at `APAddr 0x00200000` that only
  appears in the DAP's AP list AFTER that SE call, then started by register
  surgery (section 3.4 below, for the bench-only debugger-placement flow --
  this describes the 2026-07-30 run specifically; the 2026-07-31 run in
  section 2 released RTSS-HP a different way). The AP's appearance is
  evidence the SE call changed something about the cluster's debug domain --
  it is equally consistent with that domain simply being powered/clocked as
  a side effect, and is not by itself proof that the core was released to
  run; see section 3.3.

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

### 2.2 The ATOC this run depends on -- now committed to this repository

**This gap is now closed at the source level, AND re-verified on a bench**
(2026-08-04 -- see section 2.7). A two-entry ATOC (Application Table of
Contents) JSON, in the shape Alif SETOOLS' `app-gen-toc` consumes, is now
committed at
[`atoc/e1m-aen801-dualcore.json`](../atoc/e1m-aen801-dualcore.json) (see
[`atoc/README.md`](../atoc/README.md) for the full field-by-field writeup).
Reproducing the deferred-ATOC result from a clean clone is no longer
blocked on an uncommitted file, and a fresh bench run against this exact
committed shape has now happened (section 2.7) and produced a working ATOC
and 90 s/warm-reset/cold-power-off PING/PONG soaks with no gaps.

**Every field of the ATOC JSON not called out below is still TBD -- do not
invent `app-gen-toc`'s JSON schema keys or values.** The committed shape,
matching what was established below from the 2026-07-31 run, reconfirmed
2026-08-04, and completing what was previously TBD:

- Peer (REMOTE) entry, name `"ALP-HP"`: cpu_id `M55_HP`, `loadAddress
  "0x50000000"`, `flags: ["load", "boot", "deferred"]` -- this is the
  working shape that produced the 495-PING/PONG run in 2.1, and it is
  exactly what `atoc/e1m-aen801-dualcore.json` now carries. `"deferred"`
  is a valid MEMBER of the entry's `flags` ARRAY, alongside `"load"` /
  `"boot"` -- a sibling `"deferred": true` KEY is rejected by the ATOC
  builder. It sets `TOC_IMAGE_DEFERRED = 0x100` in the entry's on-the-wire
  flags word. `"ALP-HP"` matches `apps/dualcore_host/src/main.c`'s
  `DEMO_RELEASE_TOC_ENTRY_ID` literal exactly -- see `atoc/README.md`.
  **BENCH-CONFIRMED 2026-08-04**: this exact `cpu_id`/`loadAddress`/`flags`
  combination, taken from the committed file with no hand edits, produced a
  working ATOC (SES boot table flags `uLs  D`, then un-deferred to run) --
  see section 2.7.
- Host entry, name `"ALP-HE"`: cpu_id `M55_HE`, `mramAddress
  "0x80010000"`, `flags: ["boot"]` (no `"deferred"`). This entry's flags
  value was previously recorded here as TBD, inferred only from the SES
  boot table showing it boots at cold boot with no operator action.
  **BENCH-CONFIRMED 2026-08-04**: this exact `cpu_id`/`mramAddress`/`flags`
  combination produced a working ATOC (SES boot table `ALP-HE | M55-HE |
  Boot Addr 0x80010000 | ... u VB`, i.e. booted, verified) -- see
  section 2.7.
- The `DEVICE` entry's own presence, `"disabled": false`, and `"signed":
  true` are **BENCH-CONFIRMED 2026-08-04** in the sense that the write
  succeeded and the resulting package booted with this entry included
  unmodified. Its `binary` (`app-device-config.json`) is supplied by the
  Alif Security Toolkit itself, not by this repository -- see
  `atoc/README.md`'s licence-gating note. Its specific `"version":
  "0.5.00"` value remains **UNATTRIBUTED** -- no provenance for that exact
  string is recorded in this repository; it is whatever shipped in the
  SETOOLS checkout used for the 2026-08-04 run, not a value this repo
  chose or verified against Alif documentation.
- The `"version": "1.0.0"` value on the `ALP-HE`/`ALP-HP` entries is
  **BENCH-CONFIRMED to match what the SES boot table reports** (`Version
  1.0.0` in both rows, 2026-08-04) -- but WHY `"1.0.0"` specifically (as
  opposed to any other string `app-gen-toc` would accept) is
  **UNATTRIBUTED**; this repo has not established what, if anything, SES
  does with this field beyond echoing it back.
- Every other field of the ATOC JSON schema not shown in the committed
  file -- exact key-name variants `app-gen-toc` might also accept, any
  signing/certificate fields beyond the `"signed": true` shown, additional
  ATOC-level metadata -- remains **TBD** beyond what is committed. Consult
  Alif SETOOLS' `app-gen-toc` documentation for anything not shown in
  `atoc/e1m-aen801-dualcore.json` itself.

### 2.3 Build command that actually produces the proven configuration

**A default build of this repo now ships the deferred-TOC release
strategy, and this IS re-confirmed on a bench (2026-08-04, section 2.7).**
`apps/dualcore_host/Kconfig`'s `DEMO_RELEASE_STRATEGY` choice now defaults
to `CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY` (previously
`DEMO_RELEASE_VIA_START_CPU`) -- see that Kconfig's own help text for the
documented, measured reason: `alif_se_start_cpu()`'s
`SET_VTOR`/`RESET_CPU`/`RELEASE_CPU` sequence resets the M55-HP peer, which
invalidates its TCM (Alif SE Host Services API v1.109.0, p.112/p.115),
producing `CFSR = 0x00000101` (`IACCVIOL` + `IBUSERR`), `PC = 0xEFFFFFFE`.
`scripts/build-all.sh` inherits this new default too, since it does not
override `DEMO_RELEASE_STRATEGY`. No `-D` flag is needed any more to get the
deferred-TOC HOST build:

```sh
west build -p auto \
  -b e1m_aen/ae822fa0e5597ls0/rtss_he \
  -d build/ae822fa0e5597ls0/rtss_he \
  <repo>/apps/dualcore_host \
  -- \
  -DBOARD_ROOT=<repo> \
  "-DZEPHYR_EXTRA_MODULES=<repo>/modules/alif-mhuv2;<repo>/modules/alif-se-boot"
```

(`-DCONFIG_DEMO_RELEASE_VIA_TOC_ENTRY=y` is still accepted and is now a
no-op against the default, kept here only as documentation of what the
choice controls.)

**The REMOTE side needs the ITCM build, not the default MRAM build**,
because the committed ATOC's `ALP-HP` entry is a `loadAddress` (ITCM-load)
entry, not an `mramAddress` (XIP) entry -- see `atoc/README.md`'s "Which
builds feed which entry" table. Build it with the same ITCM
`-DDTC_OVERLAY_FILE` pairing section 3.1 below uses for the
debugger-placement path (semicolon-separated so the app's own board
overlay is not silently dropped):

```sh
west build -p auto \
  -b e1m_aen/ae822fa0e5597ls0/rtss_hp \
  -d build/ae822fa0e5597ls0/rtss_hp-itcm \
  <repo>/apps/dualcore_remote \
  -- \
  -DBOARD_ROOT=<repo> \
  -DZEPHYR_EXTRA_MODULES=<repo>/modules/alif-mhuv2 \
  "-DDTC_OVERLAY_FILE=boards/e1m_aen_ae822fa0e5597ls0_rtss_hp.overlay;itcm.overlay"
```

`scripts/flash-dualcore.sh` defaults to exactly these two build output
directories (`build/ae822fa0e5597ls0/rtss_he` and
`build/ae822fa0e5597ls0/rtss_hp-itcm`) when staging into a SETOOLS
checkout.

For a bench-readable record of which SE step, if any, failed, pair
`-DEXTRA_CONF_FILE=breadcrumb.conf` with the HOST build above -- the
`PROCESS_TOC_ENTRY` return value lands at global SRAM0 `0x02000040` (see
`apps/dualcore_host/Kconfig`'s `CONFIG_DEMO_EXECUTION_BREADCRUMB` help text
for the full breadcrumb map).

### 2.4 Flashing order

Build both images first, then stage both binaries and the ATOC config, then
run `app-gen-toc`/`app-write-mram` last -- the ATOC entry references both
binaries by name (`dualcore_host.bin`, `dualcore_remote.bin`), so it must be
generated and written only after both are staged where `app-gen-toc` looks
for them. `scripts/flash-dualcore.sh` does exactly this ordering: it copies
the HOST default-MRAM build and the REMOTE ITCM build into the SETOOLS
staging directory under the names `atoc/e1m-aen801-dualcore.json`
references, copies that ATOC config into the SETOOLS config directory, THEN
runs `app-gen-toc -f <config>` followed by `app-write-mram -c "$SE_UART" -p`
-- see `atoc/README.md` and the script's own header comment. This applies to
the deferred `ALP-HP` entry exactly as to the non-deferred `ALP-HE` entry:
`"deferred"` changes WHEN the SES processes the entry (at the runtime
`alif_se_process_toc_entry()` call instead of at cold boot), not whether the
image needs to already be staged/written first. **BENCH-CONFIRMED
2026-08-04** -- see section 2.7 for the `app-gen-toc`/`app-write-mram`
output.

Separately, `ALP-HE`'s `mramAddress` (`0x80010000`) must match where the
HOST Zephyr build actually links and gets flashed -- that is the board-level
`slot0_partition` fix in `boards/alp/e1m_aen/e1m_aen_ae822fa0e5597ls0_rtss_he.dts`
(section 2.4.1 below), not something `app-write-mram` reconciles on its own.
`ALP-HP`'s `loadAddress` (`0x50000000`, the M55-HP ITCM global alias) is a
runtime SES load target, not a `slot0_partition` MRAM offset -- see
`atoc/README.md`'s "Which builds feed which entry" table for why that entry
needs the ITCM-linked REMOTE build, not the default MRAM one. **Only two
files are ever staged/downloaded, not two MRAM-resident images** --
`dualcore_host.bin` (the `mramAddress` entry, XIP at `0x80010000`) and
`dualcore_remote.bin` (the `loadAddress` entry). `app-gen-toc` folds the
`loadAddress` entry INTO the generated APP TOC package rather than giving
it an MRAM address of its own -- confirmed 2026-08-04 by
`app-package-map.txt`, which showed `dualcore_host.bin` at `0x80010000`
(user-managed) and `dualcore_remote.bin` at `0x80576410` (tool-managed,
inside the APP package). This is the expected shape for a `loadAddress`
entry, not a partial flash -- see section 2.7.

### 2.4.1 Why the slot assignment is by role, not by cluster

The board files (`boards/alp/e1m_aen/e1m_aen_*_rtss_he.dts` /
`..._rtss_hp.dts`) previously assigned `slot0_partition` the other way
round: `rtss_hp` at `0x010000` and `rtss_he` at `0x300000`. That assignment
put the HOST build (`rtss_he`) at MRAM `0x80300000`, not `0x80010000` -- the
address the resident ATOC on this bench unit boots M55-HE from (section 1
above), and the address the committed `atoc/e1m-aen801-dualcore.json`'s
`ALP-HE` entry now names. With the old assignment, either the resident ATOC
would boot whatever image was actually sitting at `0x80010000` (the REMOTE
image, under the old mapping) instead of HOST, or a committed `ALP-HE` entry
pointing at `0x80010000` would not match where the HOST build actually
linked. The board files now assign `slot0_partition` by RPMsg role (HOST at
`0x010000`, REMOTE at `0x300000`), matching both the resident ATOC's boot
address and the committed ATOC's `ALP-HE` entry. **This HAS now been
re-run on silicon (2026-08-04, section 2.7)**: the SES boot table showed
`ALP-HE | M55-HE | Boot Addr 0x80010000`, and MRAM `0x80010000`'s first
four words matched `dualcore_host.bin` byte-for-byte -- confirming the
corrected slot map took effect both at build time (the reset-vector check
in `scripts/build-all.sh`'s build output, `word[1]` of `zephyr.bin` reading
`0x8001264D`) and at boot time.

**Unreconciled with the 2026-07-31 run's own HOST link address.** The
account above states the OLD slot map put the HOST build at MRAM
`0x80300000`, not `0x80010000`. The top-of-document summary states the
495-PING/PONG run (2026-07-31) was measured under that OLD slot map, using
a by-hand ATOC -- and section 1 states the resident ATOC on this bench
boots M55-HE from `0x80010000`. Both cannot hold simultaneously: an image
linked for `0x80300000` does not run correctly if XIP-booted from
`0x80010000`. This document does not resolve which account is wrong --
whether the by-hand ATOC used for the 2026-07-31 run actually pointed
`ALP-HE` at `0x80300000` (contradicting section 1's `0x80010000` claim for
that specific run), or whether the HOST build for that specific run was
not actually linked under the OLD slot map after all. **The 2026-07-31
run's actual HOST link address is therefore TBD** -- do not assume it was
either address without further evidence. This does not cast doubt on the
2026-08-04 run (section 2.7), which used the corrected slot map and the
committed ATOC together and directly confirmed both linking and booting at
`0x80010000`.

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

**Additional evidence, not a resolution: the 2026-08-04 run (section 2.7)
used the shipped default -- the deferred `ALP-HP` entry released via
`alif_se_process_toc_entry()` alone, `CONFIG_DEMO_RELEASE_TOC_THEN_BOOT`
off -- and it worked (90 s/warm-reset/cold-power-off PING/PONG soaks, no
gaps).** That is a second data point in favour of the second bullet above
(the deferred entry alone is sufficient), but it does NOT settle this
section's disagreement: the 2026-08-04 run only exercises what this repo
ships as the default, not the disputed alternative, and it was not run
with `CONFIG_DEMO_RELEASE_TOC_THEN_BOOT` enabled for comparison on the same
session. The first bullet's account (un-defer alone did NOT start the peer
executing) is not retested or retracted by this run. Treat this section as
still open pending a fresh bench run that specifically compares both
strategies back to back.

Separately from that disagreement, Alif's SE Host Services API documentation
(`SE_Host_Services_API_v1.109.0.pdf`) names the `SERVICES_boot_cpu`-style
path (`BOOT_CPU`, service 501, and the
`SERVICES_boot_reset_cpu()`/`SERVICES_boot_release_cpu()` pair it composes
with) as the one carrying the M55-HP TCM-invalidation defect recorded in
section 3.4, when the TCM is not reloaded between `RESET_CPU` and
`RELEASE_CPU` -- p.115 documents that reload as the remedy, so the same
sequence WITH the reload is documented as workable. The consequence for
this repo: `CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY`, which the 495-PING/PONG run
in 2.1 used, now ships as the DEFAULT member of the `DEMO_RELEASE_STRATEGY`
choice (previously `CONFIG_DEMO_RELEASE_VIA_START_CPU` was the default; see
2.2/2.3). `DEMO_RELEASE_VIA_START_CPU` composes
`SET_VTOR`/`RESET_CPU`/`RELEASE_CPU` against an M55-HP peer
(`AE822FA0E5597LS0`, cpu_id 2, entry `0x50000000`, as run on 2026-08-03)
WITHOUT a TCM reload -- that specific combination is the one the vendor
documents as defective for that core (see section 3.4), not a blanket
verdict against the sequence itself (section 3.7 does not declare the
`SET_VTOR` -> `RESET_CPU` -> `RELEASE_CPU` sequence wrong, and it remains
selectable in the choice for comparison). This document previously stated
the mismatch without resolving it, on the grounds that a default build
would then depend on an ATOC this repo did not commit; that ATOC is now
committed (2.2), which is what let the default change. The narrower
question this paragraph does NOT resolve -- whether the deferred entry
alone (with no subsequent `BOOT_CPU` call) is sufficient -- is the same
open disagreement recorded earlier in this section, unresolved pending a
fresh bench run.

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

### 2.7 Bench-proven under this repo's committed defaults, 2026-08-04

**This is the run that closes the reproducibility gap sections 2.2-2.4.1
above record.** Measured 2026-08-04 on **E1M-AEN801** (`AE822FA0E5597LS0`
Rev A0), flashed via this repo's own `scripts/flash-dualcore.sh` using the
committed `atoc/e1m-aen801-dualcore.json` -- no by-hand ATOC edits, no
by-hand Kconfig override. Role mapping is the same as sections 0/1: HOST on
RTSS-HE (SES-booted, SE access), REMOTE on RTSS-HP (the peer this run
releases via the deferred `ALP-HP` entry, `CONFIG_DEMO_RELEASE_TOC_ENTRY`
default, no `CONFIG_DEMO_RELEASE_TOC_THEN_BOOT` follow-up -- see the
"additional evidence" note in section 2.5).

**SES boot table after the write**, identical again after a cold power
cycle:

```
|   Name   |  CPU   | Store Addr |  Obj Addr  | Dest Addr  | Boot Addr  |   Size   |  Version  |  Flags | Time (ms)|
|   ALP-HE | M55-HE | 0x80010000 | 0x805744D0 | ---------- | 0x80010000 |    41964 |      1.0.0| u VB   |    18.55 |
|   ALP-HP | M55-HP | ---------- | 0x80575A10 | ---------- | ---------- |    39776 |      1.0.0| uLs  D |     0.00 |
Legend: (u)(C)ompressed,(L)oaded,(V)erified,(s)kipped verification,(B)ooted,(E)ncrypted,(D)eferred
```

**REMOTE console** (`uart5`, the routed side -- see section 4/README.md
section 4 for why only this side is observable on this bench), from reset:

```
*** Booting Zephyr OS build v4.4.0 ***
[00:00:00.003,000] <inf> dualcore_remote: === Alp Lab E1M-AEN dualcore demo -- REMOTE ===
[00:00:00.011,000] <inf> dualcore_remote: board target: e1m_aen/ae822fa0e5597ls0/rtss_hp, console: uart@4901d000
[00:00:00.022,000] <inf> dualcore_remote: opening ipc0 as RPMsg remote -- BLOCKS here until the host signals virtio DRIVER_OK in shared memory
[00:00:00.035,000] <inf> dualcore_remote: ipc0 open -- host is alive
[00:00:00.042,000] <inf> dualcore_remote: endpoint bound; waiting for PING
[00:00:00.049,000] <inf> dualcore_remote: PING seq=0 received; echoing PONG
[00:00:00.563,000] <inf> dualcore_remote: PING seq=1 received; echoing PONG
```

**Counts, gap-checked over parsed sequence numbers:** 90 s soak = seq 0 ->
176, 177 lines, no gaps; warm reset = seq 0 -> 39, no gaps; a 35 s cold
power-off at 16.0 V = seq 2 -> 71, no gaps (the capture socket was open
across the rail transition, so seq 0-1 were lost to line noise at
power-on, not missing from the device).

**Persistence.** MRAM `0x80010000`, four words: pre-flash `20004250
80015A51 8001F9F3 80015A3D`; post-flash `20001FB0 8001264D 80016513
80012639`; after the 35 s cold cycle, unchanged -- and byte-matching
`dualcore_host.bin`'s first four words.

**Toolchain output.** `app-gen-toc` reported `APP TOC Package size: 47920
bytes`, `APP Package Start Address: 0x805744d0`, `APP CRC32: 0xc802e862`.
`app-write-mram` warned `the SIZE of ... dualcore_host.bin is NOT multiple
of 16 bytes as required by MRAM` and padded by 4 bytes under `-p` -- this
is a warning, not an error (see `atoc/README.md`).

**Only two files are ever staged/downloaded.** `app-package-map.txt`
showed `dualcore_host.bin` at `0x80010000`, user-managed, and
`dualcore_remote.bin` at `0x80576410`, tool-managed, INSIDE the APP
package -- the `loadAddress` peer image does not get its own top-level
MRAM address, it is embedded inside the ATOC package `app-gen-toc`
generates for the `mramAddress` (HOST) entry. This is expected, given the
entry shape recorded in section 2.2, not a partial flash.

**Scope.** This run confirms the deferred-ATOC path end to end from a
clean clone, on this part, on this date, using this repo's shipped
defaults. It does NOT, by itself:

- resolve section 2.5's disagreement over whether the deferred entry alone
  is sufficient IN GENERAL (this run only exercised the shipped default --
  see the "additional evidence" note added to that section);
- say anything about E1M-AEN401 (E4), which has had no hardware run at
  all (section 4);
- re-run the section 3 debugger-placement path (unaffected by this
  change -- see section 3.4's own 2026-08-03 measurement, left intact).

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

**Flow C is not reliably repeatable back-to-back.** On this bench unit
(E1M-AEN801, `AE822FA0E5597LS0`), within the 2026-07-30 session this
section documents, one attempt ran this identical script and did not
release the peer -- `chip.hp examination failed` persisted for over 4
minutes, and RTSS-HE was found still executing the resident MRAM image
(`pc 0x8002236a`, `VTOR = 0x80010000`), not this step's freshly-loaded ITCM
image. It took a `RSetType 2; r; g` reset before Flow C worked again on
that same session. Treat a single successful Flow C run as session-local,
not as proof the next attempt will behave the same way.

### 3.3 The OpenOCD config that reaches the released HP core

RTSS-HP does not exist as a debuggable target until AFTER the SE call in
section 3.2 of the app's own boot sequence (`alif_se_boot_cpu(2,
0x50000000)`, called from the running HE image) -- its Access Port only
appears in the DAP's AP list after that call, at `APAddr 0x00200000`. That
appearance shows the SE call changed the cluster's debug-domain state (most
plausibly by powering/clocking it) -- it is not by itself proof that the
core was released to run; see section 3.4 for what was actually observed
after this OpenOCD config reaches the AP:

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

**The ordering warning, in bold, first: write the HP image only AFTER the SE
call in section 3.2 (`alif_se_boot_cpu()`, `BOOT_CPU`, `service_id` 501) has
already run, never before.** This ordering is vendor-documented for the
`RESET_CPU`/`RELEASE_CPU` path (p.115 below, scoped to Ensemble devices);
the same ordering for the `BOOT_CPU`-only path this section actually uses
remains an inference on this Ensemble part -- p.112's TCM passage is scoped
to FUSION REV_Bx devices, not Ensemble, and concerns `SERVICES_boot_cpu`
specifically, not `SERVICES_boot_reset_cpu()`/`SERVICES_boot_release_cpu()`.
Alif's SE Host Services API documentation
(`SE_Host_Services_API_v1.109.0.pdf`), p.112, on `SERVICES_boot_cpu`: "For
the M55 cores, there are cases in which this service does not work. The
currently known case is the M55-HP core in FUSION REV_Bx devices, where
resetting the core also invalidates its TCM content." The same page notes
this service "does not perform image loading, verification, etc., it just
boots the core." p.115, on `SERVICES_boot_release_cpu`: "A known case is the
M55-HP core in Ensemble devices. Because of that, after calling
`SERVICES_boot_reset_cpu()` to stop the core, the image in the TCM must be
reloaded, before calling `SERVICES_boot_release_cpu()`."

The two passages disagree on device scope -- p.112 names FUSION REV_Bx
devices, p.115 names Ensemble devices -- and this document does not resolve
that discrepancy; both are recorded here verbatim. `AE822FA0E5597LS0` (this
SoM) is an Ensemble part, so p.115's stated scope covers it directly however
p.112's is read. The vendor's own stated remedy, per p.115, is to reload the
TCM image after `SERVICES_boot_reset_cpu()` stops the core and BEFORE
calling `SERVICES_boot_release_cpu()`.

This demo releases `CONFIG_DEMO_RELEASE_PEER_CPU_ID=2`, i.e. M55_HP --
precisely the core both passages name. `alif_se_start_cpu()`
(`modules/alif-se-boot`) issues `SET_VTOR` -> `RESET_CPU` -> `RELEASE_CPU`:
`RESET_CPU` is the vendor-documented TCM-invalidating step for this core,
and `RELEASE_CPU` then releases a core whose TCM was never reloaded, since
this repo's sequence does not reload it between the two calls. That accounts
for the measured post-release fault state (`CFSR = 0x00000101` = `IACCVIOL`
+ `IBUSERR`, `HFSR = 0x40000000` = `FORCED`, `pc 0xeffffffe`) without
invoking any VTOR theory.

**Which release call each of the surrounding paragraphs is about.** The
paragraph above analyses `alif_se_start_cpu()`'s `SET_VTOR` -> `RESET_CPU`
-> `RELEASE_CPU` composition -- `CONFIG_DEMO_RELEASE_VIA_START_CPU`, the
`apps/dualcore_host` Kconfig default AT THE TIME OF THIS 2026-07-30 run
(it no longer is -- see section 2.2/2.3) -- as vendor-documented
background on why a TCM reload matters between `RESET_CPU` and
`RELEASE_CPU`. The step below (`alif_se_boot_cpu(2, 0x50000000)`, `BOOT_CPU`
alone) is the release call this section's own 2026-07-30 bench procedure
actually used, from an EARLIER `main.c` revision that called
`alif_se_boot_cpu()` alone, before the `alif_se_start_cpu()` fix landed
(see `apps/dualcore_host/src/main.c`'s top-of-file comment). Do not read the
paragraph above as a literal trace of what that earlier `BOOT_CPU`-only call
did on this run.

The practical ordering advice below is therefore supported rather than
purely inferred: reading RTSS-HP's local `0x00000000` immediately after the
SE call, before any debugger write -- on this bench unit (E1M-AEN801,
`AE822FA0E5597LS0`), within this section's 2026-07-30 run -- showed
`FEDC9ECE 511498BC CE6FBB3B EAE0460B`, not the image that may have been
written there earlier in the session. That is consistent with the
vendor-documented TCM invalidation, but is equally consistent with nothing
having been written to that address this session at all, so it does not
independently confirm the invalidation occurred. Do not rely on the HP
image being pre-loaded and left resident the way the HE image is in
section 3.2 -- write it only after this SE call has returned.

Once the peer's `alif_se_boot_cpu(2, 0x50000000)` call (`BOOT_CPU` alone,
service 501 -- the release call the HOST image used on this specific
2026-07-30 run, from a `main.c` revision predating the `alif_se_start_cpu()`
fix; NOT the `SET_VTOR`/`RESET_CPU`/`RELEASE_CPU` sequence analysed above)
has returned success and `chip.hp` is reachable per section 3.3:

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
   the core in a fault state -- observed fault state before clearing:
   `CFSR = 0x00000101` (`IACCVIOL` + `IBUSERR`), `PC = 0xEFFFFFFE`:
   - `CFSR` (`0xE000ED28`) — write back its own current value
   - `HFSR` (`0xE000ED2C`) — write back its own current value
5. **Leave `VTOR` at its reset value, `0x00000000`.** Do NOT set it to the
   image's actual link address. The core fetches SP/PC from its own local
   `0x00000000` on this path regardless of `VTOR` -- see the note in
   `apps/dualcore_host/Kconfig`'s `CONFIG_DEMO_RELEASE_PEER_ENTRY` help text
   on why the SE-reported entry address is decorative on this path.
6. `resume` -- release the core to run from the `pc`/`sp` just written.

**2026-08-03 re-run on `AE822FA0E5597LS0` (E1M-AEN801, this bench unit): the
procedure above, followed exactly as written, does not produce a working
peer.** Two independent runs on this bench both ended with the REMOTE core
printing `endpoint not bound after 5 s ...` repeatedly (49,825 repeats by
the time each session was stopped); in both runs, `grep -c "PING seq\|endpoint
bound"` against the captured (remote-side) console output returned `0` for
both strings. `PONG seq` is not a valid criterion here -- it is HOST-side
output (section 3.5) and could not appear in a REMOTE-side capture in any
run. No PING and no `endpoint bound` line appeared in either run.

After the register surgery in steps 3-5 above, the REMOTE core was read at
`xPSR 0x41000003` (`IPSR = 3`, i.e. it is in the HardFault handler)
persistently across five samples taken 500 ms apart, with `CFSR =
0x00000000` and `HFSR = 0x00000000` -- so step 4's write-1-to-clear did take
on the fault-status registers, but the core remained in HardFault anyway.
`VTOR` read `0x00000000` (as step 5 intends) and `NVIC_ISER1 = 0x00000800`
(IRQ 43 still enabled), and `pc` read `0x000064de`, which resolves to
`uart_ns16550_poll_out` (same run and part as this paragraph's opening
scope). In the same samples the HOST core (HE) was healthy:
`xPSR 0x41000000` (`IPSR = 0`, Thread mode), `CFSR` and `HFSR` both
`0x00000000`. The REMOTE's own Zephyr uptime advanced far slower than wall
clock across the session -- `[00:00:00.961,000]` after roughly 2 minutes of
wall-clock time, `[00:00:04.638,000]` after roughly 7.7 minutes.

**What this does and does not establish.** With a HardFault active, the core
executes at priority -1, which is numerically higher priority than every
maskable exception including IRQ 43 (the RPMsg doorbell), SysTick, and
PendSV -- so all three stay masked for as long as the fault is latched. That
is consistent with everything measured above: the endpoint never binds (its
IRQ never fires), Zephyr's own uptime clock advances at roughly 1% of
wall-clock time (SysTick fires only rarely, not "never" -- the uptime did
advance from `[00:00:00.961,000]` to `[00:00:04.638,000]` above, just far
slower than wall clock), and the repeating nag message is paced by whatever
UART write loop
is still reachable from the fault context rather than by the application's
intended 5-second timer (PendSV/the timer subsystem never runs). **This
document does NOT assert a cause for the latched HardFault itself**, and
does not claim an architectural reason why step 3's `xPSR = 0x01000000`
write failed to clear `IPSR` back to 0 -- neither is established by the
measurements above.

### 3.5 Expected output on both sides

HE side (host, `apps/dualcore_host` logic, running from the resident-boot
image placed per section 3.2). See the note below on which side's console
is actually observable on this bench:

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
placed and started by the debugger per section 3.4). See the note below on
which side's console is actually observable on this bench:

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
session -- see section 0. **These two sample transcripts are illustrative of
each role's OUTPUT SHAPE, not a literal capture from this bench session**:
the HOST sample above shows `board target: .../rtss_hp` and the REMOTE
sample shows `.../rtss_he`, which is the REVERSE of this bench's definitive
mapping stated in the paragraph below (HOST on `rtss_he`, REMOTE on
`rtss_hp`). Do not read the `board target:` line in either sample as a
record of which qualifier ran where on this bench -- read the message
bodies (`PONG seq=N` / `PING seq=N`) as the illustrative part.

**Which side is capturable on this bench is the OPPOSITE of what an earlier
revision of this section said.** HOST (`apps/dualcore_host`) runs on
RTSS-HE, whose board `.dts` fixes its console to `uart3`
(`boards/alp/e1m_aen/e1m_aen_ae822fa0e5597ls0_rtss_he.dts`); REMOTE
(`apps/dualcore_remote`) runs on RTSS-HP, whose console is `uart5`
(`e1m_aen_ae822fa0e5597ls0_rtss_hp.dts`) -- see root `README.md` section 4.
Only UART5 is physically routed on this bench unit; `uart3` is not. So it is
the **REMOTE's** console that is routed/capturable here, and the **HOST's**
that is not -- the reverse of the earlier claim that the HOST-side
transcript above was "the only side actually captured."

**Consequence:** the confirmation criterion this section describes above --
a captured HOST-side `PONG seq=N rtt=NN us` transcript -- is **not
obtainable from this repo unmodified on this bench**, because the HOST's own
console UART is not physically routed there.

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
  above depend on state (the SE release, the AP's post-release appearance --
  itself consistent with the cluster's debug domain simply being
  powered/clocked, and not by itself evidence that `RELEASE_CPU` succeeded,
  see section 3.3 -- and the core's fault-register contents) that another
  user's concurrent session on the same bench would silently corrupt.

### 3.7 Breadcrumb measurement, 2026-08-03

Everything in this subsection is scoped to `AE822FA0E5597LS0`,
`CONFIG_DEMO_RELEASE_PEER_CPU_ID=2` (M55_HP),
`CONFIG_DEMO_RELEASE_PEER_ENTRY=0x50000000`, this bench unit, and this date
-- it does not generalize beyond that combination. Build used
`-DEXTRA_CONF_FILE=breadcrumb.conf` (`CONFIG_DEMO_EXECUTION_BREADCRUMB=y`,
per section 2.3), with `CONFIG_DEMO_RELEASE_VIA_START_CPU=y` -- the
`apps/dualcore_host/Kconfig` default AT THE TIME OF THIS RUN, no longer the
default today (section 2.2/2.3) -- i.e. `alif_se_start_cpu()`'s decomposed
SET_VTOR / RESET_CPU / RELEASE_CPU form.

Global SRAM0 `0x02000000`-`0x02000040`, read byte-identical across three
independent debug sessions (all seventeen 32-bit words):

```
02000000 = 5A5AA5A5 A5A55A5A 5A5A5A5A 00000000
02000010 = 00000076 0000000D 000000B0 000000F0
02000020 = 00000076 0000000D 000000B0 000000F0
02000030 = 00000000 00000000 00000000 70B88523
02000040 = 940D9BA7
```

All three gate magics are present (`0x5A5AA5A5`
`BREADCRUMB_MAGIC_EARLY`, `0xA5A55A5A` `BREADCRUMB_MAGIC_PRE_RELEASE`,
`0x5A5A5A5A` `BREADCRUMB_MAGIC_POST_RELEASE`, per
`CONFIG_DEMO_EXECUTION_BREADCRUMB`'s help text), and both SE-MHU frames read
`PID0 = 0x76`, `CID0 = 0x0D` -- the genuine-ARM-MHUv2 signature that same
help text names.

**Baseline control:** a pre-run read of the same region, before the
breadcrumb build was ever run, showed `0x02000000 = C0D90A65` and
`0x02000030 = C1214F50 EBCA0219 0337C7A8 70B88523` -- different words from
the post-run dump above, confirming the changed words in the post-run dump
are real writes made by this run, not stale reads. `0x0200003C` (`70B88523`
in both the baseline and the post-run dump) and `0x02000040` correctly
stayed unwritten by this strategy -- that address is only used by the
`CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY` strategy (section 2.3), not
`CONFIG_DEMO_RELEASE_VIA_START_CPU`.

**Three zero return values:** per the `0x02000030`/`0x02000034`/`0x02000038`
map above, `alif_se_set_vtor()`, `alif_se_reset_cpu()`, and
`alif_se_release_cpu()` each returned `0`, and the whole-sequence retval at
`0x0200000C` is also `0`. All three SE calls reported success on this run.
An SE return code of `0` states only that the SE accepted and processed the
request; it says nothing on its own about the peer core's resulting state.

HP's `VTOR` (`0xE000ED08`) read `0x00000000`, `0x50000000` itself was
unreadable (`Failed to read memory at 0x50000000`), and HP's local
`0x00000000` held `0xFECC9BC2 0x510EB09D 0xAE6B1B3B 0xFAE9E70B` -- not a
plausible vector table. **This is not evidence that the `SET_VTOR` ->
`RESET_CPU` transfer failed.** This build's peer image is ITCM-linked with
its own vector table at local `0x0`; per
`apps/dualcore_host/Kconfig`'s `CONFIG_DEMO_RELEASE_PEER_ENTRY` help text,
the reported entry `0x50000000` (the ITCM global alias for the same
address) is decorative on this path -- the core fetches its initial SP/PC
from its own local `0x0` regardless of `VTOR`. So `VTOR = 0x00000000` is
consistent with the fault state recorded elsewhere in this section, but is
not by itself the cause of it. It is also, separately, a POST-ATTACH
reading, not a post-release one: the debugger's `init` printed `clearing
lockup after double fault` and `external reset detected` on first attach
(section 3.3), i.e. attaching itself perturbed the state being read. Do not
treat this reading as an unperturbed record of what `RESET_CPU` left behind,
and do not treat it as proof the `SET_VTOR` -> `RESET_CPU` transfer did not
happen.

**What this does and does not establish.** Alif documents that RESET_CPU
(service 503) transfers the value a prior SET_VTOR (service 505) staged in
the SE's Global VTOR register into the target core's own internal VTOR
register -- see `modules/alif-se-boot/include/alif_se_boot.h`'s
`alif_se_reset_cpu()` doc comment. That transfer was **not observed** on
`AE822FA0E5597LS0`, cpu_id 2 (M55_HP), entry `0x50000000`, on this date: the
three SE calls all reported success, but the only VTOR reading taken was
post-attach and therefore confounded by the debugger's own attach-time state
clearing, so it does not by itself prove the transfer did not happen either.
This subsection does not declare the SET_VTOR -> RESET_CPU -> RELEASE_CPU
sequence wrong, and does not propose switching to `alif_se_boot_cpu()`
(`BOOT_CPU`, service 501) alone as a fix -- `apps/dualcore_host/src/main.c`'s
top-of-file comment (`main.c:61-76`) already records that call measured
failing on this exact silicon on an earlier run.

## 4. Known gaps / not yet closed

- **CLOSED 2026-08-04.** The ATOC file behind the 2.1 result is committed
  to this repository (`atoc/e1m-aen801-dualcore.json`), the shipped default
  builds the deferred-TOC strategy, and this exact combination, together
  with the corrected MRAM slot map, HAS now been run on a bench -- see 2.7
  for the full record. (The original 2026-07-31 495-PING/PONG result itself
  was still measured under the OLD, uncommitted-ATOC/non-default-strategy/
  old-slot-map conditions -- see 2.2, 2.3, 2.4.1 for that distinction; it is
  the 2026-08-04 run in 2.7 that confirms the committed combination.)
- **Whether the deferred entry alone releases the peer, or a separate
  `BOOT_CPU` call is also required, is an open disagreement** between
  `apps/dualcore_host/Kconfig`'s help text and the 2.1 result -- see 2.5.
  The 2026-08-04 run (2.7) adds a second data point FOR "alone is
  sufficient" (it used the shipped default, deferred entry alone, and
  worked), but did not compare both strategies back to back, so this
  remains unresolved pending a fresh bench run that does.
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
- **A 2026-08-03 re-run of the section-3.4 debugger-placement procedure,
  exactly as documented, measured `0` for both `PING seq` and `endpoint
  bound` across two attempts** (the REMOTE core repeating `endpoint not
  bound after 5 s ...` instead) -- see the new measurement recorded in
  section 3.4. This remote-side capture is not comparable to the 2026-07-30
  run's HOST-side `PONG seq` count (section 0, section 3.5); the two are
  different sides of the link. The core was read persistently HardFaulted
  after the register surgery; no cause for the fault is established.
- **Flow C (section 3.2) is not reliably repeatable back-to-back** -- one
  attempt did not release the peer and needed a `RSetType 2; r; g` reset
  before a subsequent attempt worked.
- **`alif_se_start_cpu()`'s SET_VTOR -> RESET_CPU -> RELEASE_CPU sequence
  returns success on `AE822FA0E5597LS0` (cpu_id 2, entry `0x50000000`,
  2026-08-03) without the vendor-documented VTOR transfer being confirmed**
  -- see section 3.7. The only VTOR reading taken was post-attach and so is
  not conclusive either way.
