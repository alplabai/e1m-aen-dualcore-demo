# E1M-AEN dual-core demo (Cortex-M55 RTSS-HP + RTSS-HE)

Two Cortex-M55 clusters on an Alp Lab E1M-AEN SoM exchanging RPMsg PING/PONG
over shared memory, with an Alif MHUv2 mailbox pair as the doorbell.

Built from **upstream Zephyr v4.4.0** plus this repo's own out-of-tree
modules. No vendor SDK, no Alp Lab SDK, no proprietary component at build
time.

Verified on **E1M-AEN801** (`AE822FA0E5597LS0`) on 2026-08-04: 177
consecutive PING/PONG exchanges with no gaps, surviving a cold power cycle
with no debugger attached. That is one bench unit, not a general guarantee.

---

## 1. What you need

| | |
|---|---|
| **Board** | E1M-AEN801 SoM (`AE822FA0E5597LS0`) on a carrier |
| **Zephyr** | a west workspace at **v4.4.0** — this repo's `west.yml` fetches it |
| **Toolchain** | Zephyr SDK (arm-zephyr-eabi) |
| **Flashing** | **Alif Security Toolkit** (`app-gen-toc`, `app-write-mram`) |
| **Console** | a USB-serial connection to the carrier's routed UART |

> **The Alif Security Toolkit is licence-gated and is NOT redistributed in
> this repository.** Obtain it from Alif. You can build everything here
> without it; you cannot flash without it.

## 2. Get the sources

```sh
west init -m https://github.com/alplabai/e1m-aen-dualcore-demo
west update
```

This creates a workspace with upstream Zephyr v4.4.0 and this repo alongside
it.

## 3. Build

Two images — these are the two the flashing step in section 4 consumes.
`<repo>` is the absolute path to this checkout.

**HOST** (runs on RTSS-HE):

```sh
west build -p auto \
  -b e1m_aen/ae822fa0e5597ls0/rtss_he \
  -d build/ae822fa0e5597ls0/rtss_he \
  <repo>/apps/dualcore_host \
  -- \
  -DBOARD_ROOT=<repo> \
  "-DZEPHYR_EXTRA_MODULES=<repo>/modules/alif-mhuv2;<repo>/modules/alif-se-boot"
```

**REMOTE, ITCM-linked** (runs on RTSS-HP — this is the one the flashing step
needs):

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

> The REMOTE has a second, MRAM-linked build (`-d .../rtss_hp`, no
> `itcm.overlay`). **That one is not what you flash.** The ATOC entry for the
> peer is an ITCM-load entry, so it needs the ITCM build. Feeding the wrong
> one is the easiest way to waste a bench cycle; `scripts/flash-dualcore.sh`
> checks for it and refuses.

`scripts/build-all.sh ae822fa0e5597ls0` builds the two default targets, but
does **not** produce the ITCM REMOTE variant — build that one directly with
the command above.

## 4. Flash

```sh
# SETOOLS_DIR is the toolkit's INNER directory -- the one that contains
# app-gen-toc, app-write-mram, build/ and utils/. On a stock extraction that
# is <wherever-you-unpacked-it>/app-release-exec-linux, not the folder above it.
export SETOOLS_DIR=/path/to/app-release-exec-linux
export SE_UART=/dev/ttyUSB0          # the SE-UART on your carrier

scripts/flash-dualcore.sh \
  build/ae822fa0e5597ls0/rtss_he \
  build/ae822fa0e5597ls0/rtss_hp-itcm
```

This stages both images and [`atoc/e1m-aen801-dualcore.json`](atoc/e1m-aen801-dualcore.json)
into the toolkit, runs `app-gen-toc` to build one combined package, and
writes it with `app-write-mram`.

> **Flashing writes MRAM and overwrites whatever image is currently in
> slot0.**

## 5. What you should see

On the REMOTE console (`uart5` — the side actually routed on this repo's own
bench carrier), from power-on:

```
*** Booting Zephyr OS build v4.4.0 ***
<inf> dualcore_remote: === Alp Lab E1M-AEN dualcore demo -- REMOTE ===
<inf> dualcore_remote: ipc0 open -- host is alive
<inf> dualcore_remote: endpoint bound; waiting for PING
<inf> dualcore_remote: PING seq=0 received; echoing PONG
<inf> dualcore_remote: PING seq=1 received; echoing PONG
```

A PING roughly every 500 ms, sequence numbers with no gaps. It comes back on
its own after a power cycle — no debugger.

The HOST prints the matching `PONG seq=N rtt=NN us` lines on **its** console
(`uart3`), which may not be wired to an accessible connector on your own
carrier. If you only see the REMOTE side, that is expected — see section 7.

## 6. How it works

The resident ATOC boots **M55-HE** first, so that cluster has Secure Enclave
access and runs the **HOST** role. The **M55-HP** peer is described by a
second ATOC entry flagged `deferred`: the Secure Enclave stages its image but
deliberately does not start it. At runtime the HOST releases the peer by
un-deferring that entry through the Secure Enclave
(`SERVICES_boot_process_toc_entry`, service 500). Then the RPMsg link opens
and PING/PONG begins.

> **The peer entry must be flagged `deferred`.** Releasing an M55-HP peer
> through the plain boot/reset path does not work on this part — resetting
> that core invalidates its TCM, so it starts from empty memory. The
> committed ATOC already has the flag; if you write your own, keep it.

Apps are named by **role**, not by cluster (`dualcore_host` /
`dualcore_remote`), because which physical cluster runs the host is a
per-silicon fact. Both apps build for both cluster qualifiers.

## 7. Board targets and console

| Board target | SoM | Console UART |
|---|---|---|
| `e1m_aen/ae822fa0e5597ls0/rtss_he` | E1M-AEN801 | `uart3` (`uart@4901b000`) |
| `e1m_aen/ae822fa0e5597ls0/rtss_hp` | E1M-AEN801 | `uart5` (`uart@4901d000`) |
| `e1m_aen/ae402fa0e5597le0/rtss_he` | E1M-AEN401 | `uart3` (`uart@4901b000`) |
| `e1m_aen/ae402fa0e5597le0/rtss_hp` | E1M-AEN401 | `uart5` (`uart@4901d000`) |

This deliberately differs from the upstream `ensemble_e8_dk` reference board,
which uses `uart2`/`uart4`. **The E1M-AEN carrier pinout has not been
confirmed against a schematic** — check which UART your carrier actually
breaks out before assuming a console is dead.

## 8. MRAM map

MRAM is 5632 KB (`0x580000`) at base `0x80000000`. Slots are assigned **by
role**, so the HOST lands where the resident ATOC boots M55-HE:

| Region | Offset | Size | Contents |
|---|---|---|---|
| reserved | `0x000000`-`0x00FFFF` | 64 KB | not part of either slot |
| `slot0_partition` (`rtss_he`) | `0x010000`-`0x2FFFFF` | 3008 KB | HOST image, links at `0x80010000` |
| `slot0_partition` (`rtss_hp`) | `0x300000`-`0x55FFFF` | 2432 KB | REMOTE image (MRAM build) |
| reserved | `0x560000`-`0x57FFFF` | 128 KB | ATOC application table |

> **Do not flash an image built for `boards/alif/ensemble_e8_dk` onto
> hardware also running an image from this board.** That board gives each
> cluster the entire 5632 KB, so the layouts are incompatible and one image
> silently destroys the other.

Note the flashing flow in section 4 does **not** write the REMOTE to
`0x300000` — the peer is ITCM-loaded from inside the ATOC package. That
partition exists for the REMOTE's own MRAM build, which this flow does not
use.

## 9. Limitations

- **E1M-AEN401 (`ae402fa0e5597le0`, E4) has never been run on silicon.**
  Everything measured here is E1M-AEN801 (E8). Its boot-cluster mapping is
  assumed to match, not confirmed — do not rely on it.
- The MHU base addresses (`0x400B0000` TX, `0x400A0000` RX), IRQ 43 and the
  `sram_ipc0` carve-out (`0x02010000`, 64 KB) are validated on E8 only.
- The carrier pinout is unconfirmed against a schematic (section 7).
- The debugger-placement alternative documented in
  `docs/BENCH-DUALCORE.md` section 3 does **not** currently produce a working
  link — use the ATOC path above.
- For E4 targets, check the J-Link device string before your first flash:
  upstream maps `AE402FA0E5597LE0` to a device string ending `LS0`, not
  `LE0`.

## 10. Further reading

- [`atoc/README.md`](atoc/README.md) — the ATOC entries, which build feeds
  which entry, and the toolkit's staging layout.
- [`docs/BENCH-DUALCORE.md`](docs/BENCH-DUALCORE.md) — the full bring-up
  record: every bench run, the measured failures and what they ruled out, the
  Secure-Enclave call sequences, and the open questions.
- `modules/alif-mhuv2/`, `modules/alif-se-boot/` — the two out-of-tree
  modules, both written against the register interfaces directly with no
  vendor HAL beneath them.

## Licence

Apache-2.0 (`SPDX-License-Identifier: Apache-2.0`), per the SPDX headers on
individual files. Copyright 2026 Alp Lab AB.
