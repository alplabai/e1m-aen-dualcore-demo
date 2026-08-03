# E1M-AEN dual-core (Cortex-M55 RTSS-HP + RTSS-HE) demo

> **What is proven, and on what.** A dual-core RPMsg ping/pong link between
> both Cortex-M55 clusters ran on **E1M-AEN801** (`ae822fa0e5597ls0`)
> silicon on 2026-07-30: 369 consecutive PONGs, no gaps. That run used the
> bench-proven role->core mapping this repo now builds by default -- HOST
> on `rtss_he`, REMOTE on `rtss_hp` -- because the resident ATOC on that
> bench unit boots M55-HE first and only that cluster has Secure Enclave
> access. See [`docs/BENCH-DUALCORE.md`](docs/BENCH-DUALCORE.md) for the
> full procedure, including section 0's explanation of why this is the
> MIRROR of what the app names `dualcore_hp`/`dualcore_he` used to imply
> (apps are now named `dualcore_host`/`dualcore_remote`, by role, for
> exactly this reason), and its final section for exactly what is and is
> not reproducible from this tree today. The E1M-AEN401 (`ae402fa0e5597le0`)
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
| `e1m_aen/ae402fa0e5597le0/rtss_hp` | E1M-AEN401 (primary) | AE402FA0E5597LE0 | `uart4` (`uart@4901c000`) |
| `e1m_aen/ae402fa0e5597le0/rtss_he` | E1M-AEN401 (primary) | AE402FA0E5597LE0 | `uart2` (`uart@4901a000`) |
| `e1m_aen/ae822fa0e5597ls0/rtss_hp` | E1M-AEN801 (bench silicon) | AE822FA0E5597LS0 | `uart4` (`uart@4901c000`) |
| `e1m_aen/ae822fa0e5597ls0/rtss_he` | E1M-AEN801 (bench silicon) | AE822FA0E5597LS0 | `uart2` (`uart@4901a000`) |

This mirrors the console assignment on the upstream `ensemble_e8_dk`
reference board exactly. **The E1M-AEN carrier pinout has not been confirmed
against a schematic** -- every board `.dts` file that fixes `uart4`/`uart2`
as the console carries a comment saying so.

## 5. MRAM partition map

The MRAM is 5632 KB (`0x580000` bytes) starting at base address `0x80000000`.
Upstream `ensemble_e8_dk` gives **both** clusters `slot0_partition: reg =
<0x0 DT_SIZE_K(5632)>` -- i.e. each cluster's image claims the *entire*
MRAM. Flashing two images built against the upstream board files onto the
same chip means the second image overwrites the first.

This board splits the MRAM instead, so both cluster images can coexist:

| Region | Offset (bytes) | Size | Contents |
|---|---|---|---|
| `slot0_partition` (rtss_hp) | `0x000000`-`0x2FFFFF` | 3072 KB | RTSS-HP image |
| `slot0_partition` (rtss_he) | `0x300000`-`0x55FFFF` | 2432 KB | RTSS-HE image |
| reserved, unpartitioned | `0x560000`-`0x57FFFF` | 128 KB | Headroom for the Alif SETOOLS/ATOC application table |

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
    debugger attached. **This path has not been exercised in this
    project.**
  - A debugger-attached start (halt one core, load/step the other via
    J-Link), which does not require ATOC but also does not survive a power
    cycle on its own.
- **Ordering matters if you use the SETOOLS/ATOC path:** build both images
  first, then write both MRAM slots (`slot0_partition` for `rtss_hp` at
  `0x000000`, then `slot0_partition` for `rtss_he` at `0x300000`), then
  write the ATOC last -- the ATOC entry references both slots, so it must
  be written only after both slots it points to already exist in MRAM.

## 8. Known gaps / not yet verified

- The E1M-AEN carrier pinout is **not confirmed** against a schematic; the
  console UART assignment (`uart4` for RTSS-HP, `uart2` for RTSS-HE) is
  inherited from the Alif Ensemble E8 DevKit reference board, not from an
  E1M-AEN board bring-up.
- The MHU base addresses (`0x400B0000` TX, `0x400A0000` RX), IRQ 43, and the
  `sram_ipc0` shared-memory carve-out (`0x02010000`, 64 KB) were validated
  against the **E1M-AEN801 (E8 / `AE822FA0E5597LS0`)** silicon actually on
  the bench. They are **unverified on E4 / E1M-AEN401** silicon.
- **One hardware run has been performed by this project**, on E1M-AEN801
  (E8 / `AE822FA0E5597LS0`) silicon, 2026-07-30 -- see the banner above and
  `docs/BENCH-DUALCORE.md`. That run used a debugger-driven placement
  procedure, not a standalone SETOOLS/ATOC boot; the SETOOLS/ATOC path
  above remains unexercised, and E1M-AEN401 (E4) has had no hardware run
  at all. Everything else above reflects device-tree/Kconfig-level
  verification against upstream Zephyr sources and the schematics/register
  documentation available at authoring time, not a bench bring-up log.
