# dualcore_he -- RPMsg REMOTE demo (rtss_he)

Runs on the `rtss_he` (Cortex-M55, up to 160 MHz) cluster of the Alp Lab
E1M-AEN SoM. Console is `uart3` (E1M UART1, Alif pads P1_3/P1_2, E1M pads AG4/AH4). This is the RPMsg **remote** half of the
dual-core demo; its counterpart is `../dualcore_hp`, the RPMsg **host**
running on the same silicon's `rtss_hp` cluster.

See the project root `README.md` for the full picture (board layout, MRAM
partitioning, the out-of-tree `alif-mhuv2` mailbox module, and the caveats
section on what has/hasn't been exercised on real hardware).

## What it does

1. Opens the `ipc0` IPC-service instance and registers one endpoint
   (`dualcore_ping_pong`) -- the same name `dualcore_hp` registers, so the
   two endpoints bind to each other.
2. Waits for the endpoint to bind to its `dualcore_hp` counterpart, logging
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

```sh
west build -p auto \
  -b e1m_aen/ae402fa0e5597le0/rtss_he \
  -d build/ae402fa0e5597le0/rtss_he \
  <repo>/apps/dualcore_he \
  -- \
  -DBOARD_ROOT=<repo> \
  -DZEPHYR_EXTRA_MODULES=<repo>/modules/alif-mhuv2
```

Substitute `ae822fa0e5597ls0` for the E1M-AEN801 bench silicon target. Flash
alongside a `dualcore_hp` image built for the matching `rtss_hp` target and
the same SoC -- see the root `README.md`'s MRAM partition warning before
flashing either image over an `ensemble_e8_dk` build.
