# dualcore_hp -- RPMsg HOST demo (rtss_hp)

Runs on the `rtss_hp` (Cortex-M55, up to 400 MHz) cluster of the Alp Lab
E1M-AEN SoM. Console is `uart5` (E1M UART0, Alif pads P3_5/P3_4, E1M pads F2/G2). This is the RPMsg **host** half of the
dual-core demo; its counterpart is `../dualcore_he`, the RPMsg **remote**
running on the same silicon's `rtss_he` cluster.

See the project root `README.md` for the full picture (board layout, MRAM
partitioning, the out-of-tree `alif-mhuv2` mailbox module, and the caveats
section on what has/hasn't been exercised on real hardware).

## What it does

1. Opens the `ipc0` IPC-service instance and registers one endpoint
   (`dualcore_ping_pong`).
2. Waits for the endpoint to bind to its `dualcore_he` counterpart, logging
   a "still waiting" message every 5 s if the remote hasn't come up yet.
3. Every 500 ms, sends a `struct ping_pong_msg { uint32_t seq; }` PING and
   blocks for the matching PONG, then logs the sequence number and the
   round-trip time in microseconds (measured with this core's own cycle
   counter -- both send and receive timestamps are taken on `rtss_hp`, so
   no cross-core clock synchronization is needed).

Every `ipc_service_*` return code is checked explicitly; a send failure logs
and retries the same sequence number rather than silently dropping it.

## Build

```sh
west build -p auto \
  -b e1m_aen/ae402fa0e5597le0/rtss_hp \
  -d build/ae402fa0e5597le0/rtss_hp \
  <repo>/apps/dualcore_hp \
  -- \
  -DBOARD_ROOT=<repo> \
  -DZEPHYR_EXTRA_MODULES=<repo>/modules/alif-mhuv2
```

Substitute `ae822fa0e5597ls0` for the E1M-AEN801 bench silicon target. Flash
alongside a `dualcore_he` image built for the matching `rtss_he` target and
the same SoC -- see the root `README.md`'s MRAM partition warning before
flashing either image over an `ensemble_e8_dk` build.
