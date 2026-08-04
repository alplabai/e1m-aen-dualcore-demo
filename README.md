# E1M-AEN dual-core (Cortex-M55 RTSS-HP + RTSS-HE) demo

> **Bench-proven, 2026-08-04: the deferred-ATOC path now runs from a clean
> clone on E1M-AEN801.** Using this repo's own `scripts/flash-dualcore.sh`
> against the committed
> [`atoc/e1m-aen801-dualcore.json`](atoc/e1m-aen801-dualcore.json) -- no
> by-hand ATOC, no by-hand Kconfig override -- on **E1M-AEN801**
> (`AE822FA0E5597LS0` Rev A0): the SES boot table showed both entries
> resident and booting (`ALP-HE` at `Boot Addr 0x80010000`, flags `u VB`;
> `ALP-HP` flags `uLs  D`), identical again after a cold power cycle; the
> REMOTE console (the physically routed side, `uart5`) showed uninterrupted
> PING/PONG -- 177 lines/no gaps over a 90 s soak, no gaps across a warm
> reset, no gaps across a 35 s cold power-off at 16.0 V; and MRAM
> `0x80010000` held `dualcore_host.bin`'s own first four words, byte-for-byte,
> unchanged by the cold power cycle. This closes the reproducibility gap
> this banner previously recorded here -- a clean clone plus the
> licence-gated Alif Security Toolkit now runs the demo. Full transcripts,
> the `app-gen-toc`/`app-write-mram` output, and two operational details
> worth knowing before you try it yourself (the padding warning on a
> non-16-byte binary, and why only two files are ever staged even though
> the ATOC has two boot-relevant entries) are in
> `docs/BENCH-DUALCORE.md` section 2.7.
>
> **What this run does NOT settle.** It exercises this repo's shipped
> default -- the deferred `ALP-HP` entry released via
> `alif_se_process_toc_entry()` alone, `CONFIG_DEMO_RELEASE_TOC_THEN_BOOT`
> off (`default n`) -- and that worked, which is evidence for that shape
> (recorded in section 2.5 below). It does NOT resolve section 2.5's
> separate, older disagreement over whether the deferred entry ALONE is
> sufficient IN GENERAL, versus a case needing a follow-up `BOOT_CPU` call:
> this run only exercised the shipped default, not the disputed
> alternative, so both contradicting bench accounts already in this tree
> still stand.
>
> **What is proven, and on what.** Three dual-core RPMsg ping/pong runs have
> now been performed on **E1M-AEN801** (`ae822fa0e5597ls0`) silicon:
>
> - **2026-08-04, the reproducibility-closing run (see above and
>   `docs/BENCH-DUALCORE.md` section 2.7):** the deferred-ATOC path
>   exercised end to end from a clean clone -- committed ATOC, default
>   Kconfig strategy, corrected MRAM slot map, `scripts/flash-dualcore.sh`
>   -- with no by-hand ATOC and no by-hand Kconfig override. Survives a
>   power cycle, no debugger attached -- the shape a customer carrier would
>   actually ship, and now the one a clean clone actually reproduces.
> - **2026-07-31, the first deferred-ATOC result:** releasing the peer core
>   via the same **deferred SETOOLS/ATOC entry** mechanism (Secure Enclave
>   `service_id` 500, `SERVICES_boot_process_toc_entry`, wrapped as
>   `alif_se_process_toc_entry()`) -- **495 consecutive PING/PONG
>   round-trips over 4m11s, no drop, no gap.** This first run used a by-hand
>   ATOC and a by-hand Kconfig override, under the OLD (pre-fix) MRAM slot
>   map, not this repo's current defaults -- the 2026-08-04 run above is
>   what closes that gap.
> - **2026-07-30, a bench-only alternative:** the peer core placed and
>   started by a debugger -- 369 consecutive PONGs, no gaps, **HOST-side
>   output captured from the pre-rename scratch build** (before the
>   `dualcore_hp`/`dualcore_he` -> `dualcore_host`/`dualcore_remote` rename;
>   see `docs/BENCH-DUALCORE.md` section 0). This repo, unmodified, cannot
>   capture that same side on this bench today -- the HOST role's console
>   (`uart3`) is not physically routed there (section 4 below). Does not
>   survive a power cycle and needs a debugger permanently attached; not a
>   production path. See the 2026-08-03 re-run of this same procedure in
>   section 8 below and `docs/BENCH-DUALCORE.md` section 3.4.
>
> All three runs used the bench-proven role->core mapping this repo builds
> by default -- HOST on `rtss_he`, REMOTE on `rtss_hp` -- because the
> resident ATOC on that bench unit boots M55-HE first and only that cluster
> has Secure Enclave access. See [`docs/BENCH-DUALCORE.md`](docs/BENCH-DUALCORE.md)
> for the full procedure: section 0 explains why this is the MIRROR of what
> the app names `dualcore_hp`/`dualcore_he` used to imply (apps are now
> named `dualcore_host`/`dualcore_remote`, by role, for exactly this
> reason); section 2 is the primary deferred-ATOC procedure, now
> bench-confirmed end to end by section 2.7's 2026-08-04 run, with the
> section 2.5 disagreement over whether the deferred entry alone always
> suffices still open; section 3 is the debugger-placement alternative;
> section 4 lists what remains open. The E1M-AEN401 (`ae402fa0e5597le0`)
> boot-cluster mapping has never been observed on real silicon -- do not
> assume it matches E1M-AEN801's.

## 1. What this is

A standalone Zephyr + west demo of the two Cortex-M55 clusters on Alp Lab's
E1M-AEN SoM family exchanging messages over IPC. Primary target is the
**E1M-AEN401** SoM (silicon `AE402FA0E5597LE0`, Alif Ensemble E4). Also
supported is the **E1M-AEN801** SoM (silicon `AE822FA0E5597LS0`, Alif
Ensemble E8) -- this is the silicon actually available on the bench for
validation, so it is the one this project has been checked against so far.

Each SoC carries two independent Cortex-M55 clusters:

- **RTSS-HP** ("High Performance"), up to 400 MHz
- **RTSS-HE** ("High Efficiency"), up to 160 MHz

Both clusters boot their own Zephyr image out of the same on-chip MRAM and
talk to each other over a shared-memory OpenAMP link, using an Alif MHUv2
mailbox pair as the doorbell. Before the RPMsg link opens, the **HOST**
role (`apps/dualcore_host`) asks the Alif Secure Enclave to release its
peer, the **REMOTE** role (`apps/dualcore_remote`), from reset (see
`modules/alif-se-boot/`). Which physical cluster runs HOST is a
per-silicon fact, not a repo convention: it is whichever cluster the
resident ATOC boots first and so has Secure Enclave access -- on the
E1M-AEN801 bench unit that is `rtss_he`, not `rtss_hp` (see
`docs/BENCH-DUALCORE.md` section 0). Apps are named by role
(`dualcore_host`/`dualcore_remote`), not by cluster, and each builds for
both `rtss_hp` and `rtss_he` qualifiers so the mapping is a build-target
choice, not a source change -- see section 6. Upstream Zephyr provides no
in-tree mechanism to bring a peer cluster out of reset on its own (section
7).

## 2. No alp-sdk dependency

This project does **not** depend on alp-sdk, on any path, in any build
target. It is built from four things only:

1. Upstream Zephyr **v4.4.0** (`zephyrproject-rtos/zephyr`), fetched via the
   `west.yml` in this repo.
2. The out-of-tree board definition in `boards/alp/e1m_aen/` (this repo).
3. The out-of-tree MHUv2 mailbox module in `modules/alif-mhuv2/` (this
   repo) -- a from-scratch `sys_read32`/`sys_write32` driver against the
   Alif MHUv2 mailbox register windows, with no HAL and no vendor SDK
   underneath it.
4. The out-of-tree Secure-Enclave BOOT_CPU client in `modules/alif-se-boot/`
   (this repo) -- authored from the SE service protocol, not from Alif's
   `se_services` sources (see that module's README); HOST-only, used by
   `apps/dualcore_host` to release its REMOTE peer before opening the IPC
   link.

Nothing here reaches into an `alp-sdk-dev` checkout at build time. Alp Lab's
own Apache-2.0 mailbox driver source was copied out of that repo by hand
during development (see `modules/alif-mhuv2/`), but the finished tree has
zero build-time or run-time reference back to it.

## 3. What upstream Zephyr v4.4.0 does (and does not) give you for Alif Ensemble

Upstream Zephyr v4.4.0 already carries the Alif Ensemble SoC layer
(`soc/alif/ensemble/{e4,e6,e8,e1c}/`) and device tree family
(`dts/arm/alif/ensemble/`), including both SoCs this project targets
(`ae402fa0e5597le0` and `ae822fa0e5597ls0`) and both CPU clusters
(`rtss_hp`, `rtss_he`). No `hal_alif` module and no Alif-maintained Zephyr
fork are needed to build for either SoC -- confirmed by reading
`soc/alif/ensemble/common/soc.c` and every `soc/alif/ensemble/*/CMakeLists.txt`
in the v4.4.0 tree, none of which reference a HAL module.

What upstream does **not** provide is nearly as important. The entire Alif
Ensemble peripheral device-tree surface
(`dts/arm/alif/ensemble/common/ensemble_common.dtsi`, 126 lines) declares
**only**:

- a `pinctrl` node (`compatible = "alif,pinctrl"`)
- `uart0` through `uart5` (NS16550-compatible, at `0x49018000`-`0x4901d000`)
- the `peripheral_region` container they live in

There is **no GPIO, no I2C, no SPI, no ADC, no PWM, no timer, no MHU, and no
Ethos-U node anywhere in `dts/arm/alif/`.** That is why this demo is
UART-console-only, and why the MHUv2 mailbox driver that the IPC link
depends on could not be taken from upstream and had to be supplied
out-of-tree in `modules/alif-mhuv2/`.

## 4. Board targets and console assignment

| Board target | SoM | Silicon | Console UART |
|---|---|---|---|
| `e1m_aen/ae402fa0e5597le0/rtss_hp` | E1M-AEN401 (primary) | AE402FA0E5597LE0 | `uart5` (`uart@4901d000`) |
| `e1m_aen/ae402fa0e5597le0/rtss_he` | E1M-AEN401 (primary) | AE402FA0E5597LE0 | `uart3` (`uart@4901b000`) |
| `e1m_aen/ae822fa0e5597ls0/rtss_hp` | E1M-AEN801 (bench silicon) | AE822FA0E5597LS0 | `uart5` (`uart@4901d000`) |
| `e1m_aen/ae822fa0e5597ls0/rtss_he` | E1M-AEN801 (bench silicon) | AE822FA0E5597LS0 | `uart3` (`uart@4901b000`) |

This does **not** mirror the upstream `ensemble_e8_dk` reference board's
console assignment -- that board uses `uart2` (rtss_he) / `uart4` (rtss_hp).
This board deliberately fixes `uart3`/`uart5` instead (see the
`zephyr,console` property in each `boards/alp/e1m_aen/*.dts` file). **The
E1M-AEN carrier pinout has not been confirmed against a schematic.**
`boards/alp/e1m_aen/e1m_aen-pinctrl.dtsi` documents that both UART5 (E1M
UART0, pads F2/G2) and UART3 (E1M UART1, pads AG4/AH4) reach the E1M
module's own edge connector -- that is a SoM-level pad-routing fact, not a
claim about any specific carrier board. What IS confirmed is a separate,
THIS-BENCH-CARRIER-level fact: on the E1M-AEN801 bench carrier specifically
(not the SoM), which of the two edge-connector UARTs that carrier's own
board actually breaks out to somewhere a bench operator can reach it.
**UART5 is the console that is actually routed on that bench carrier;
`uart3` is not routed to an accessible connector there.** This does not
contradict the SoM edge-connector pad table above -- it is a statement about
this one carrier board's downstream wiring, not about the module's pads.
Because on that bench the
HOST role runs on `rtss_he` (console `uart3`) and the REMOTE role runs on
`rtss_hp` (console `uart5`) -- see section 1 and
`docs/BENCH-DUALCORE.md` section 0 -- the HOST side's console is not
observable on that bench unit; only the REMOTE side's is.

## 5. MRAM partition map

The MRAM is 5632 KB (`0x580000` bytes) starting at base address `0x80000000`.
Upstream `ensemble_e8_dk` gives **both** clusters `slot0_partition: reg =
<0x0 DT_SIZE_K(5632)>` -- i.e. each cluster's image claims the *entire*
MRAM. Flashing two images built against the upstream board files onto the
same chip means the second image overwrites the first.

This board splits the MRAM instead, so both cluster images can coexist. Slots
are assigned **by RPMsg role, not by cluster**, to match where the resident
ATOC on the E1M-AEN801 bench unit actually boots: it boots M55-HE at
MRAM-XIP address `0x80010000` (see `docs/BENCH-DUALCORE.md` section 1), and
RTSS-HE runs the HOST role, so the HOST image occupies the lower slot and the
REMOTE image the upper one -- the reverse of a naive "first cluster gets the
first slot" assignment. This is also the address the committed ATOC's
`ALP-HE` entry names (`atoc/e1m-aen801-dualcore.json`'s `mramAddress`):

| Region | Offset (bytes) | Size | Contents |
|---|---|---|---|
| reserved, unpartitioned | `0x000000`-`0x00FFFF` | 64 KB | Not part of either slot0 -- see `boards/alp/e1m_aen/*.dts` comments for why |
| `slot0_partition` (rtss_he) | `0x010000`-`0x2FFFFF` | 3008 KB | HOST image (RTSS-HE) -- links at `0x80010000`, matching the resident ATOC's boot address |
| `slot0_partition` (rtss_hp) | `0x300000`-`0x55FFFF` | 2432 KB | REMOTE image (RTSS-HP) |
| reserved, unpartitioned | `0x560000`-`0x57FFFF` | 128 KB | Headroom for the Alif SETOOLS/ATOC application table |

Arithmetic, abutting and non-overlapping: `0x10000 + DT_SIZE_K(3008)` =
`0x10000 + 0x2F0000` = `0x300000` (where the REMOTE slot starts); `0x300000 +
DT_SIZE_K(2432)` = `0x300000 + 0x260000` = `0x560000` (where the REMOTE slot
ends and the 128 KB ATOC headroom begins).

**WARNING:** do not flash an image built for `boards/alif/ensemble_e8_dk`
onto hardware also running an image from this board. The upstream board
gives both clusters the full 5632 KB, so the two layouts are incompatible
and flashing one after the other silently destroys the first image.

## 6. Build commands

Both roles are built as separate west application builds, each pointed at
this repo's out-of-tree board directory and module(s) via `-DBOARD_ROOT` and
`-DZEPHYR_EXTRA_MODULES`. Substitute `<repo>` for the absolute path of this
checkout and `<zephyr-base>` for your Zephyr v4.4.0 checkout.

Each app (`apps/dualcore_host`, `apps/dualcore_remote`) builds for BOTH the
`rtss_hp` and `rtss_he` qualifiers of BOTH SoCs -- eight combinations total.
The commands below are the bench-proven default on E1M-AEN801
(`ae822fa0e5597ls0`): HOST on `rtss_he`, REMOTE on `rtss_hp` (see the banner
above and `docs/BENCH-DUALCORE.md` section 0). The other qualifier for each
app is equally buildable -- just swap `rtss_he`/`rtss_hp` in the `-b` and
app-path arguments -- but is not this SoC's bench-confirmed mapping.

HOST needs BOTH out-of-tree modules, semicolon-separated: `modules/alif-mhuv2`
for the doorbell/RPMsg link, and `modules/alif-se-boot` (see section 1/2)
because `apps/dualcore_host/prj.conf` sets `CONFIG_ALIF_SE_BOOT=y` and every
board overlay in that app instantiates that module's `alplab,e8-se-boot`
devicetree node -- omitting it fails Kconfig with `attempt to assign the
value 'y' to the undefined symbol ALIF_SE_BOOT`:

```sh
west build -p auto \
  -b e1m_aen/ae822fa0e5597ls0/rtss_he \
  -d build/ae822fa0e5597ls0/rtss_he \
  <repo>/apps/dualcore_host \
  -- \
  -DBOARD_ROOT=<repo> \
  "-DZEPHYR_EXTRA_MODULES=<repo>/modules/alif-mhuv2;<repo>/modules/alif-se-boot"
```

REMOTE needs only `modules/alif-mhuv2`:

```sh
west build -p auto \
  -b e1m_aen/ae822fa0e5597ls0/rtss_hp \
  -d build/ae822fa0e5597ls0/rtss_hp \
  <repo>/apps/dualcore_remote \
  -- \
  -DBOARD_ROOT=<repo> \
  -DZEPHYR_EXTRA_MODULES=<repo>/modules/alif-mhuv2
```

**The REMOTE role also needs a SEPARATE, ITCM-linked build for the
SETOOLS/ATOC flashing path in section 7** -- the committed ATOC's `ALP-HP`
entry is a `loadAddress` (ITCM-load) entry, not an `mramAddress` (XIP) one
(see `atoc/README.md`'s "Which builds feed which entry" table), so it needs
an image linked to run from the M55-HP ITCM, not the default MRAM-XIP build
above. `scripts/build-all.sh` does **not** produce this variant -- build it
directly:

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

`scripts/flash-dualcore.sh` defaults to exactly this build output directory
(`build/ae822fa0e5597ls0/rtss_hp-itcm`) alongside the HOST's
`build/ae822fa0e5597ls0/rtss_he` from section 7 below -- see
`docs/BENCH-DUALCORE.md` section 2.3 for the same command with the
reasoning attached.

For the E1M-AEN401 / `ae402fa0e5597le0` SoC, swap the board string and app
arguments accordingly -- its boot-cluster mapping is unconfirmed, so
`scripts/build-all.sh` applies the same default (HOST on `rtss_he`, REMOTE
on `rtss_hp`) to it for simplicity, not because it has been bench-checked
there too. `scripts/build-all.sh` runs the default mapping for you and
prints which qualifier it picked as HOST/REMOTE and why:

```sh
export ZEPHYR_BASE=<zephyr-base>
scripts/build-all.sh                 # E1M-AEN401 only (host->rtss_he, remote->rtss_hp)
scripts/build-all.sh ae822fa0e5597ls0  # E1M-AEN801 only (bench-proven mapping)
scripts/build-all.sh all             # all four board targets, both SoCs
```

**The HOST command above now builds with the deferred-ATOC release strategy
by default** -- `apps/dualcore_host/Kconfig`'s `DEMO_RELEASE_STRATEGY`
choice now defaults to `CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY`, so no extra
`-D` flag is needed for that part. **This build, paired with the ATOC
written from the committed `atoc/e1m-aen801-dualcore.json` (see
`atoc/README.md` and section 7 below), is now bench-proven** -- measured
2026-08-04 on E1M-AEN801 (`AE822FA0E5597LS0` Rev A0) via
`scripts/flash-dualcore.sh`, see `docs/BENCH-DUALCORE.md` section 2.7. The
earlier 495-PING/PONG result in the banner above (2026-07-31) was measured
under the OLD default (`DEMO_RELEASE_VIA_START_CPU`), overridden by hand at
build time, and an ATOC built by hand -- the 2026-08-04 run is what
confirms this repo's own committed defaults, not that first hand-built one.
See `docs/BENCH-DUALCORE.md` sections 2.2, 2.3, and 2.7 for the full
account.

## 7. Flashing and running

This section is deliberately honest about what has **not** been exercised
yet.

- **J-Link device-string discrepancy for AE402FA0E5597LE0.** Upstream
  `boards/alif/ensemble_e8_dk/board.cmake` maps
  `CONFIG_SOC_AE402FA0E5597LE0_RTSS_HP`/`_HE` to J-Link device strings
  `AE402FA0E5597LS0_M55_HP` / `AE402FA0E5597LS0_M55_HE` -- note `LS0`, not
  `LE0`. This is upstream's own device string, not a typo introduced here.
  **Check this against your `JLinkExe` device list before the first flash**
  of an E1M-AEN401 (E4) target; do not assume `LE0` will resolve.
- **Releasing RTSS-HE from reset has no in-tree mechanism.** Upstream
  Zephyr does not provide a way to bring the HE cluster out of reset from
  the HP cluster (or vice versa) as part of a normal `west flash`. The two
  realistic paths are:
  - **Alif SETOOLS/ATOC** (`app-gen-toc` + `app-write-mram`): this is the
    only option that gives a standalone, power-on-and-run demo with no
    debugger attached. **This path HAS been exercised on the bench, in this
    repo's exact committed, shipped-default form** (2026-08-04,
    E1M-AEN801 `AE822FA0E5597LS0` Rev A0: `scripts/flash-dualcore.sh`
    against the committed `atoc/e1m-aen801-dualcore.json`, no by-hand
    edits -- see `docs/BENCH-DUALCORE.md` section 2.7 for the full record).
    It was first exercised on 2026-07-31 (495 consecutive PING/PONG
    round-trips, via a **deferred** ATOC entry released at runtime through
    SE `service_id` 500, `SERVICES_boot_process_toc_entry` -- see
    `docs/BENCH-DUALCORE.md` section 2.1), but that first run used an ATOC
    built by hand, against the OLD (pre-fix) slot map and the OLD
    (non-default) Kconfig strategy. The source-level gaps that first run
    left open -- the ATOC JSON at
    [`atoc/e1m-aen801-dualcore.json`](atoc/e1m-aen801-dualcore.json) (see
    [`atoc/README.md`](atoc/README.md)), and `apps/dualcore_host/Kconfig`'s
    `DEMO_RELEASE_STRATEGY` choice defaulting to
    `DEMO_RELEASE_VIA_TOC_ENTRY` -- are what the 2026-08-04 run confirms
    together, from a clean clone. Use `scripts/flash-dualcore.sh` to stage
    and write it once you have both images built (section 6) and an Alif
    Security Toolkit checkout (the toolkit itself is licence-gated and is
    NOT redistributed here).
  - A debugger-attached start (halt one core, load/step the other via
    J-Link), which does not require ATOC but also does not survive a power
    cycle on its own -- see `docs/BENCH-DUALCORE.md` section 3.
- **Ordering matters if you use the SETOOLS/ATOC path, but it is NOT "write
  two MRAM slots, then write the ATOC".** The HOST and REMOTE roles are
  staged and written through the SAME `app-gen-toc`/`app-write-mram` step,
  not written to MRAM independently first, and the REMOTE role is never
  written to `slot0_partition`'s `0x300000` offset by this flow at all:
    1. Build both images (section 6): the HOST's default MRAM build
       (`build/ae822fa0e5597ls0/rtss_he`) and the REMOTE's **ITCM** build
       (`build/ae822fa0e5597ls0/rtss_hp-itcm`) -- NOT the REMOTE's default
       MRAM build. See "Which build feeds which ATOC entry" below for why.
    2. Stage both `.bin`s and `atoc/e1m-aen801-dualcore.json` into the Alif
       Security Toolkit checkout (`scripts/flash-dualcore.sh` does this).
    3. Run `app-gen-toc` against that config -- it builds ONE combined APP
       TOC package containing the HOST's MRAM-XIP entry (`ALP-HE`,
       `mramAddress 0x80010000`) and the REMOTE's ITCM-load entry (`ALP-HP`,
       `loadAddress 0x50000000`) together. The REMOTE's image ends up
       embedded INSIDE that package (confirmed in `app-package-map.txt`,
       2026-08-04 -- see `docs/BENCH-DUALCORE.md` section 2.7), not at a
       `slot0_partition` MRAM offset of its own.
    4. Run `app-write-mram` to write the resulting package to MRAM, once,
       last -- not two separate slot writes. `"deferred"` changes WHEN the
       SES processes the `ALP-HP` entry (at the runtime
       `alif_se_process_toc_entry()` call instead of at cold boot), not
       whether the package needs to already be written first.
  `boards/alp/e1m_aen/*_rtss_hp.dts`'s `slot0_partition` at `0x300000` (see
  section 5) is real devicetree partitioning -- it is what the REMOTE's OWN
  default, non-ITCM build links against -- but the SETOOLS/ATOC flow this
  repo ships does not use that build or that address for the REMOTE role;
  it uses the ITCM build, loaded into ITCM by SES at runtime, never flashed
  to any MRAM offset of its own. **Do not write a REMOTE image to
  `0x300000` expecting the resident ATOC to boot it there** -- nothing in
  this repo's committed ATOC references that address. See
  `docs/BENCH-DUALCORE.md` section 2.4 for the full account.
- **Which build feeds which ATOC entry is documented precisely in
  `atoc/README.md`'s "Which builds feed which entry" table** -- `ALP-HE`
  takes the default MRAM HOST build, `ALP-HP` takes the ITCM REMOTE build.
  Swapping them is the easiest way to waste a bench cycle.

## 8. Known gaps / not yet verified

- The E1M-AEN carrier pinout is **not confirmed** against a schematic; the
  console UART assignment (`uart5` for RTSS-HP, `uart3` for RTSS-HE) does
  NOT match the Alif Ensemble E8 DevKit reference board (which uses `uart4`/
  `uart2`) -- see section 4. On the E1M-AEN801 bench unit, UART5 is the
  physically routed console; `uart3` is not routed there.
- The MHU base addresses (`0x400B0000` TX, `0x400A0000` RX), IRQ 43, and the
  `sram_ipc0` shared-memory carve-out (`0x02010000`, 64 KB) were validated
  against the **E1M-AEN801 (E8 / `AE822FA0E5597LS0`)** silicon actually on
  the bench. They are **unverified on E4 / E1M-AEN401** silicon.
- **Three hardware runs have been performed by this project**, all on
  E1M-AEN801 (E8 / `AE822FA0E5597LS0`) silicon -- see the banner above and
  `docs/BENCH-DUALCORE.md`: a debugger-driven placement run (2026-07-30,
  369 consecutive PONGs, `docs/BENCH-DUALCORE.md` section 3), a standalone
  SETOOLS/ATOC run via a deferred entry, by hand (2026-07-31, 495
  consecutive PING/PONG round-trips, `docs/BENCH-DUALCORE.md` section 2.1),
  and the same SETOOLS/ATOC mechanism exercised from a clean clone under
  this repo's committed defaults (2026-08-04,
  `docs/BENCH-DUALCORE.md` section 2.7). E1M-AEN401 (E4) has had no
  hardware run at all. Everything else above reflects device-tree/
  Kconfig-level verification against upstream Zephyr sources and the
  schematics/register documentation available at authoring time, not a
  bench bring-up log.
- **RESOLVED 2026-08-04: the SETOOLS/ATOC run now reproduces from a clean
  clone.** The two source-level gaps that previously blocked this -- the
  ATOC JSON not being committed, and the Kconfig default not shipping the
  deferred-TOC strategy -- were closed in source, and on 2026-08-04 the
  full combination (committed `atoc/e1m-aen801-dualcore.json`, the
  `CONFIG_DEMO_RELEASE_VIA_TOC_ENTRY` default, and the corrected MRAM slot
  map from section 5) was exercised together on E1M-AEN801
  (`AE822FA0E5597LS0` Rev A0) via `scripts/flash-dualcore.sh`, with no
  by-hand ATOC and no by-hand Kconfig override -- see
  `docs/BENCH-DUALCORE.md` section 2.7 for the full measured record. This
  does **NOT** resolve section 2.5's separate, still-open disagreement over
  whether the deferred entry alone releases the peer or a separate
  `BOOT_CPU` call is also required in general -- the 2026-08-04 run only
  exercised this repo's shipped default (deferred entry alone, no
  follow-up call), which is evidence for that shape but not a resolution
  of the broader disagreement; see section 2.5 for both contradicting
  bench accounts, still unresolved pending a fresh bench run that
  specifically targets that question.
- **On the section-3 debugger-placement path, a fresh 2026-08-03 attempt
  measured `0` for both `PING seq` and `endpoint bound` in the captured
  remote-side console output, alongside 49,825 repeats of `endpoint not
  bound after 5 s ...`.** Two runs of the documented procedure both left the
  REMOTE core printing that line repeatedly; `grep -c "PING seq\|endpoint
  bound"` against the captured (remote-side) console returned `0` for
  both -- the endpoint never bound and no PING ever arrived. See
  `docs/BENCH-DUALCORE.md` section 3.4.
- **The section-3.4 register-surgery procedure, as documented, does not
  recover the core.** After the surgery, the REMOTE core was read
  persistently HardFaulted (`xPSR 0x41000003`) across five samples 500 ms
  apart, with its Zephyr uptime advancing at roughly 1% of wall-clock time.
  See `docs/BENCH-DUALCORE.md` section 3.4.
- **The `alif_se_start_cpu()` SET_VTOR -> RESET_CPU -> RELEASE_CPU sequence
  returns success on this silicon, but the only VTOR reading taken
  afterwards does not confirm the documented VTOR transfer.** On a
  2026-08-03 breadcrumb run (`AE822FA0E5597LS0`,
  `CONFIG_DEMO_RELEASE_PEER_CPU_ID=2`,
  `CONFIG_DEMO_RELEASE_PEER_ENTRY=0x50000000`), all three SE calls returned
  `0`. The peer's `VTOR` register read `0x00000000`, but that reading was
  taken POST-ATTACH (confounded by the debugger's own attach-time state
  clearing) and this build's peer image is ITCM-linked with its vector
  table at local `0x0`, so a `0x00000000` VTOR reading is uninformative
  either way -- **this is not evidence that the transfer failed.** See
  `docs/BENCH-DUALCORE.md` section 3.7.
- **The root cause of the `-116` SE-transport failure the in-flight PR's
  D-cache-maintenance change targeted is NOT established.** That change's
  own stated rationale has been disproved against the linked ELF's
  `mem_attr_region` table -- see `docs/BENCH-DUALCORE.md` section 2.6. The
  failure may recur with no known fix.
