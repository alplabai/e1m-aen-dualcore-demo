# alif_mhuv2 -- out-of-tree Zephyr module

A standalone Zephyr module providing a Zephyr MBOX-class driver for the ARM
MHUv2 (Message Handling Unit, v2) as integrated in the Alif Ensemble family
(E1/E3/E5/E7/E8). It backs the inter-core doorbell signalling used by the
OpenAMP static-vrings RPMsg IPC path between the two on-chip M55 clusters
(RTSS-HP / RTSS-HE) on the E1M-AEN dual-core demo.

This module has **no dependency on alp-sdk** -- it is pure upstream Zephyr
(`zephyr/kernel.h`, `zephyr/drivers/mbox.h`, `zephyr/sys/sys_io.h`, ...) plus
`sys_read32`/`sys_write32` against the MHUv2 register map. No vendor HAL is
linked.

## What it provides

| Piece | Path |
|---|---|
| Kconfig symbol | `CONFIG_MBOX_ALIF_MHUV2` |
| DT compatible | `alif,mhuv2-mbox` |
| Driver source | `drivers/mbox/mbox_alif_mhuv2.c` |
| DT binding | `dts/bindings/mbox/alif,mhuv2-mbox.yaml` |

Tier: **ADR-0017-ADJACENT** (novel Alif MHUv2 doorbell IP; no upstream Zephyr
driver, no fork driver, and no vendor HAL library exists to consume, so it was
authored from the ARM MHUv2 spec, DDI 0515, as a last resort). See the file
header in `drivers/mbox/mbox_alif_mhuv2.c` for the full provenance and the
bench-validation note (E1M-AEN801, dual-M55 OpenAMP RPMsg ping/pong).

The devicetree compatible is deliberately `alif,mhuv2-mbox`, NOT `arm,mhuv2`,
so it never collides with the `arm,mhuv2` binding shipped by the opt-in
`alifsemi/zephyr_alif` fork -- this module builds cleanly on plain upstream
Zephyr with no fork and no `hal_alif` module present.

## Model

- One devicetree node = one MHUv2 frame, sender ("tx") OR receiver ("rx"),
  selected by the `alif,direction` string property, `reg` size `0x1000`.
- Doorbell (signalling) mode only: no payload travels through the MHU itself.
  `mtu_get()` always returns 0; real data rides shared SRAM via the OpenAMP
  vrings (see the sibling `sram_ipc0` memory-region carve-out used by the
  dual-core apps in this repo).
- `#mbox-cells = <1>`: the cell is the doorbell bit index (0..31) inside
  channel-window 0. Only window 0 is used.

## How to point Zephyr at this module

Pass this module's path via `ZEPHYR_EXTRA_MODULES`, either on the `west build`
command line or in a board/app `CMakeLists.txt`/`prj.conf`-adjacent build
config:

```sh
west build -b <board_target> <app_dir> \
    -- -DZEPHYR_EXTRA_MODULES=/path/to/e1m-aen-dualcore-demo/modules/alif-mhuv2
```

Zephyr's module machinery then:

- runs `CMakeLists.txt` (module root, per `zephyr/module.yml`'s
  `build.cmake: .`) to compile `drivers/mbox/mbox_alif_mhuv2.c` when
  `CONFIG_MBOX_ALIF_MHUV2` is set,
- sources `Kconfig` (module root, per `build.kconfig: Kconfig`) into the
  Kconfig tree, which in turn `rsource`s `drivers/Kconfig` ->
  `drivers/mbox/Kconfig` to declare `CONFIG_MBOX_ALIF_MHUV2`
  (`default y if DT_HAS_ALIF_MHUV2_MBOX_ENABLED`, `depends on MBOX`), and
- adds `dts/bindings` to the devicetree binding search path (per
  `zephyr/module.yml`'s `settings.dts_root: .`), so a board/overlay `.dts`
  node with `compatible = "alif,mhuv2-mbox";` resolves against
  `dts/bindings/mbox/alif,mhuv2-mbox.yaml`.

Multiple `ZEPHYR_EXTRA_MODULES` paths can be semicolon-separated if this repo
carries other out-of-tree modules alongside this one.

## Directory layout

```
modules/alif-mhuv2/
+-- zephyr/module.yml           # module manifest (cmake/kconfig/dts_root)
+-- CMakeLists.txt               # module root -- add_subdirectory(drivers)
+-- Kconfig                      # module root -- rsource drivers/Kconfig
+-- drivers/
|   +-- CMakeLists.txt           # add_subdirectory(mbox)
|   +-- Kconfig                  # rsource mbox/Kconfig
|   +-- mbox/
|       +-- CMakeLists.txt       # zephyr_library() + sources_ifdef
|       +-- Kconfig              # CONFIG_MBOX_ALIF_MHUV2
|       +-- mbox_alif_mhuv2.c    # the driver
+-- dts/bindings/mbox/
    +-- alif,mhuv2-mbox.yaml     # the DT binding
```
