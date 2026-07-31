# dualcore_host -- RPMsg HOST demo

Named by RPMsg **role**, not by M55 cluster: this app is the RPMsg **host**
half of the dual-core demo on WHICHEVER cluster it is built for. Its
counterpart is `../dualcore_remote`, the RPMsg **remote**, running on the
SAME silicon's other cluster.

It is deliberately buildable for **both** the `rtss_hp` and `rtss_he` board
qualifiers, because which physical M55 cluster must run the host is a
per-silicon fact, not a repo convention: the host is whichever cluster the
Secure Enclave boots first and so has SE access (see the root `README.md`
and `docs/BENCH-DUALCORE.md` section 0). On the E1M-AEN801
(`ae822fa0e5597ls0`) bench unit that is **`rtss_he`** -- the mirror of what
this app's old `dualcore_hp` name implied. The E1M-AEN401
(`ae402fa0e5597le0`) mapping has never been observed on real silicon; both
qualifiers are kept buildable for it too. See `scripts/build-all.sh` and
`docs/BENCH-DUALCORE.md` section 0 for exactly which is bench-proven where.

See the project root `README.md` for the full picture (board layout, MRAM
partitioning, the out-of-tree `alif-mhuv2` mailbox module, and the caveats
section on what has/hasn't been exercised on real hardware).

## What it does

1. Asks the Alif Secure Enclave to release its peer core
   (`alif_se_start_cpu()`, see `Kconfig` and `src/main.c`'s top-of-file
   comment for which core and why) before opening `ipc0`. Every outcome
   (success, local transport failure, SE-reported error) is logged and
   never aborts the demo -- this side keeps running as host even if the
   release fails.
2. Opens the `ipc0` IPC-service instance and registers one endpoint
   (`dualcore_ping_pong`).
3. Waits for the endpoint to bind to its `dualcore_remote` counterpart,
   logging a "still waiting" message every 5 s if the remote hasn't come up
   yet.
4. Every 500 ms, sends a `struct ping_pong_msg { uint32_t seq; }` PING and
   blocks for the matching PONG, then logs the sequence number and the
   round-trip time in microseconds (measured with this core's own cycle
   counter -- both send and receive timestamps are taken on this core, so
   no cross-core clock synchronization is needed).

Every `ipc_service_*` return code is checked explicitly; a send failure logs
and retries the same sequence number rather than silently dropping it.

## Build

This app needs BOTH out-of-tree modules, semicolon-separated:
`modules/alif-mhuv2` for the doorbell/RPMsg link, and `modules/alif-se-boot`
because `prj.conf` sets `CONFIG_ALIF_SE_BOOT=y` and every board overlay in
this app instantiates that module's `alplab,e8-se-boot` devicetree node --
omitting it fails Kconfig with `attempt to assign the value 'y' to the
undefined symbol ALIF_SE_BOOT`.

Bench-proven default on the E1M-AEN801 bench unit (`ae822fa0e5597ls0`):

```sh
west build -p auto \
  -b e1m_aen/ae822fa0e5597ls0/rtss_he \
  -d build/ae822fa0e5597ls0/rtss_he \
  <repo>/apps/dualcore_host \
  -- \
  -DBOARD_ROOT=<repo> \
  "-DZEPHYR_EXTRA_MODULES=<repo>/modules/alif-mhuv2;<repo>/modules/alif-se-boot"
```

The other qualifier (`rtss_hp`) and the other SoC (`ae402fa0e5597le0`, either
qualifier -- unconfirmed boot order) build the same way, substituting the
board string and build directory. Flash alongside a `dualcore_remote` image
built for the OTHER qualifier of the same SoC -- see the root `README.md`'s
MRAM partition warning before flashing either image over an `ensemble_e8_dk`
build. `scripts/build-all.sh` builds every combination for you and prints
which qualifier it picked as host and why.

## Bench RAM console (`ram-console.conf`)

This app's console is UART by default (see `prj.conf`), which is right for a
customer carrier but not always observable on a given bench (e.g. only UART5
is physically routed here, while `rtss_he`'s console is `uart3`). For that
case, `ram-console.conf` is an opt-in fragment that switches the console to
`CONFIG_RAM_CONSOLE`, backed by a fixed `ram_console_buf` symbol a debugger
can read over SWD regardless of UART wiring. Turn it on by adding
`-DEXTRA_CONF_FILE=ram-console.conf` to the `west build` invocation above;
while it's applied, UART console output is disabled.
