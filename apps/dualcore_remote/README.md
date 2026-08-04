# dualcore_remote -- RPMsg REMOTE demo

Named by RPMsg **role**, not by M55 cluster: this app is the RPMsg
**remote** half of the dual-core demo on WHICHEVER cluster it is built for.
Its counterpart is `../dualcore_host`, the RPMsg **host**, running on the
SAME silicon's other cluster.

It is deliberately buildable for **both** the `rtss_hp` and `rtss_he` board
qualifiers, because which physical M55 cluster runs the remote is a
per-silicon fact, not a repo convention -- it is whichever cluster the
Secure Enclave does NOT boot first (see the root `README.md` and
`docs/BENCH-DUALCORE.md` section 0). On the E1M-AEN801 (`ae822fa0e5597ls0`)
bench unit that is **`rtss_hp`** -- the mirror of what this app's old
`dualcore_he` name implied. The E1M-AEN401 (`ae402fa0e5597le0`) mapping has
never been observed on real silicon; both qualifiers are kept buildable for
it too. See `scripts/build-all.sh` and `docs/BENCH-DUALCORE.md` section 0
for exactly which is bench-proven where.

See the project root `README.md` for the full picture (board layout, MRAM
partitioning, the out-of-tree `alif-mhuv2` mailbox module, and the caveats
section on what has/hasn't been exercised on real hardware).

## What it does

1. Opens the `ipc0` IPC-service instance and registers one endpoint
   (`dualcore_ping_pong`) -- the same name `dualcore_host` registers, so the
   two endpoints bind to each other.
2. Waits for the endpoint to bind to its `dualcore_host` counterpart, logging
   a "still waiting" message every 5 s if the host hasn't come up yet.
3. On every received `struct ping_pong_msg { uint32_t seq; }` PING, logs it
   and immediately echoes the identical struct straight back as the PONG.
   It keeps no sequence-number state of its own -- the host owns the
   sequence and detects gaps or reordering on its side.

The echo send happens from a dedicated thread woken by a semaphore given in
the endpoint's `received` callback, not from inside the callback itself --
the same pattern every upstream `ipc_service` sample uses (see
`zephyr/samples/subsys/ipc/ipc_service/*/remote/src/main.c`).

## Build

This app needs only `modules/alif-mhuv2` -- it never enables
`CONFIG_ALIF_SE_BOOT` and never instantiates the `alplab,e8-se-boot`
devicetree node (that's host-only, see `../dualcore_host`).

Bench-proven default on the E1M-AEN801 bench unit (`ae822fa0e5597ls0`):

```sh
west build -p auto \
  -b e1m_aen/ae822fa0e5597ls0/rtss_hp \
  -d build/ae822fa0e5597ls0/rtss_hp \
  <repo>/apps/dualcore_remote \
  -- \
  -DBOARD_ROOT=<repo> \
  -DZEPHYR_EXTRA_MODULES=<repo>/modules/alif-mhuv2
```

The other qualifier (`rtss_he`) and the other SoC (`ae402fa0e5597le0`, either
qualifier -- unconfirmed boot order) build the same way, substituting the
board string and build directory. Flash alongside a `dualcore_host` image
built for the OTHER qualifier of the same SoC -- see the root `README.md`'s
MRAM partition warning before flashing either image over an `ensemble_e8_dk`
build. `scripts/build-all.sh` builds every combination for you and prints
which qualifier it picked as remote and why.

## SEPARATE ITCM build, needed for the SETOOLS/ATOC flashing path

The default MRAM build above is **not** what the committed
`atoc/e1m-aen801-dualcore.json` needs for this app's role. That ATOC's
`ALP-HP` entry is a `loadAddress` (ITCM-load) entry, not an `mramAddress`
(XIP) one -- see `atoc/README.md`'s "Which builds feed which entry" table
-- so it needs an image linked to run from the M55-HP ITCM instead:

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
(`build/ae822fa0e5597ls0/rtss_hp-itcm`) when staging into a SETOOLS
checkout -- see the root `README.md` section 7 and
`docs/BENCH-DUALCORE.md` sections 2.3 and 2.7 (this exact build is what
produced the bench-proven 2026-08-04 run).
