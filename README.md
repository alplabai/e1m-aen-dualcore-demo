# E1M-AEN dual-core (Cortex-M55 RTSS-HP + RTSS-HE) demo

> **Proven on hardware.** A dual-core RPMsg ping/pong link between both
> Cortex-M55 clusters ran on E1M-AEN801 silicon on 2026-07-30 (369
> consecutive PONGs, no gaps). See
> [`docs/BENCH-DUALCORE.md`](docs/BENCH-DUALCORE.md) for the exact,
> ordered reproduction procedure -- it corrects an assumption the rest of
> this README still makes in places (section 7 in particular): that
> RTSS-HP always has SE access and boots second. On the bench that was
> the other way around; `docs/BENCH-DUALCORE.md` section 0 explains the
> mismatch.

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
mailbox pair as the doorbell. Before the RTSS-HP cluster opens that link, it
first asks the Alif Secure Enclave to release RTSS-HE from reset (see
`modules/alif-se-boot/`) -- RTSS-HE has no SE access of its own, so it cannot
release itself, and upstream Zephyr provides no other in-tree mechanism to
bring it out of reset (section 7).

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
   `se_services` sources (see that module's README); HP-only, used by
   `apps/dualcore_hp` to release RTSS-HE before opening the IPC link.

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

Both cores are built as separate west application builds, each pointed at
this repo's out-of-tree board directory and module(s) via `-DBOARD_ROOT` and
`-DZEPHYR_EXTRA_MODULES`. Substitute `<repo>` for the absolute path of this
checkout and `<zephyr-base>` for your Zephyr v4.4.0 checkout.

RTSS-HP (primary target, E1M-AEN401 / `ae402fa0e5597le0`) needs BOTH
out-of-tree modules, semicolon-separated: `modules/alif-mhuv2` for the
doorbell/RPMsg link, and `modules/alif-se-boot` (see section 1/2) because
`apps/dualcore_hp/prj.conf` sets `CONFIG_ALIF_SE_BOOT=y` and the HP board
overlay instantiates that module's `alplab,e8-se-boot` devicetree node --
omitting it fails Kconfig with `attempt to assign the value 'y' to the
undefined symbol ALIF_SE_BOOT`:

```sh
west build -p auto \
  -b e1m_aen/ae402fa0e5597le0/rtss_hp \
  -d build/ae402fa0e5597le0/rtss_hp \
  <repo>/apps/dualcore_hp \
  -- \
  -DBOARD_ROOT=<repo> \
  "-DZEPHYR_EXTRA_MODULES=<repo>/modules/alif-mhuv2;<repo>/modules/alif-se-boot"
```

RTSS-HE (primary target, E1M-AEN401 / `ae402fa0e5597le0`):

```sh
west build -p auto \
  -b e1m_aen/ae402fa0e5597le0/rtss_he \
  -d build/ae402fa0e5597le0/rtss_he \
  <repo>/apps/dualcore_he \
  -- \
  -DBOARD_ROOT=<repo> \
  -DZEPHYR_EXTRA_MODULES=<repo>/modules/alif-mhuv2
```

For the E1M-AEN801 / `ae822fa0e5597ls0` bench silicon, swap the board string
and app arguments accordingly (`e1m_aen/ae822fa0e5597ls0/rtss_hp` and
`e1m_aen/ae822fa0e5597ls0/rtss_he`, same apps). `scripts/build-all.sh` runs
all of the above for you:

```sh
export ZEPHYR_BASE=<zephyr-base>
scripts/build-all.sh                 # E1M-AEN401 only (rtss_hp + rtss_he)
scripts/build-all.sh ae822fa0e5597ls0  # E1M-AEN801 only
scripts/build-all.sh all             # all four targets
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
- **No hardware run has been performed by this project.** Everything above
  reflects device-tree/Kconfig-level verification against upstream Zephyr
  sources and the schematics/register documentation available at
  authoring time, not a bench bring-up log.
