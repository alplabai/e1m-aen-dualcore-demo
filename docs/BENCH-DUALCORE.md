# Reproducing the proven E1M-AEN801 dual-core RPMsg run

This document is the exact, ordered procedure that produced a live RPMsg
ping/pong link between the two Cortex-M55 clusters on E1M-AEN801 silicon on
**2026-07-30**: 369 consecutive `PONG seq=N rtt=32..35 us` lines, no gaps,
still running at `seq=563` when the session ended. This is the only
dual-core run this project has performed on real hardware to date.

**Read this before assuming the bench shape matches the repo's board-naming
convention.** It does not, exactly -- see step 0.

## 0. The bench shape is the MIRROR of the repo's `rtss_hp`/`rtss_he` naming

The repo's `boards/alp/e1m_aen/*.dts` and `apps/dualcore_hp`/`apps/dualcore_he`
were written under the assumption that the `rtss_hp` cluster is the one with
Secure Enclave (SE) access and boots first. **That assumption did not hold on
the bench.** What was actually observed:

- The resident ATOC (Application Table of Contents) already on the bench
  silicon boots **RTSS-HE** first: `ALP-HE | M55-HE | Boot Addr 0x80010000`.
  RTSS-HE is therefore the **SES-booted core** on this bench, not RTSS-HP.
- **RTSS-HE runs the RPMsg HOST** role and has SE access on this bench: it
  calls `alif_se_boot_cpu(2 /* M55-HP, EXTSYS_0 */, 0x50000000)` to release
  RTSS-HP.
- **RTSS-HP runs the RPMsg REMOTE** role. It is released by the SE call
  above, then loaded by a debugger (NOT by the SE/ATOC) into its own LOCAL
  `0x00000000` through an Access Port (AP) at `APAddr 0x00200000` that only
  appears in the DAP's AP list AFTER the release, then started by register
  surgery (step 5 below).

Because of this mismatch, reproducing the run with THIS repo's images means:

- Build `apps/dualcore_hp` for `e1m_aen/ae822fa0e5597ls0/rtss_hp` as normal
  (its Kconfig defaults, `CONFIG_DEMO_RELEASE_PEER_CPU_ID=2` /
  `CONFIG_DEMO_RELEASE_PEER_ENTRY=0x50000000`, already encode the
  bench-proven release parameters -- see `apps/dualcore_hp/Kconfig` and
  `apps/dualcore_hp/src/main.c`'s top-of-file comment).
- On the bench, this HOST logic executed on the RTSS-**HE** cluster's silicon
  (the SES-booted one), calling out to release RTSS-**HP**. The operator did
  this with a hand-written scratch host app rather than flashing
  `apps/dualcore_hp` itself onto RTSS-HE (the `se_boot` devicetree node this
  repo's `CONFIG_ALIF_SE_BOOT` depends on is only instantiated in
  `apps/dualcore_hp`'s `rtss_hp` overlays -- see
  `modules/alif-se-boot/README.md`). Reconciling that -- so `west build ... -b
  e1m_aen/<soc>/rtss_hp` produces an image that is actually flashable to
  whichever cluster the bench's resident ATOC boots first -- is tracked as
  follow-up work, **out of scope for this document**, which describes the
  procedure exactly as run.
- What IS reproducible today from this repo, unmodified: the RTSS-HP-side
  half of the exchange (`apps/dualcore_he`'s REMOTE logic, ITCM-retargeted
  per step 2 below, placed and started via the debugger per steps 4-5) and
  the Kconfig-correct release call in `apps/dualcore_hp` (step 3), against a
  resident host built and placed by the same debugger-driven method as the
  remote, following the same steps.

## 1. Which core boots first, and why

The bench silicon already carries a resident ATOC that boots **M55-HE**
(RTSS-HE) at MRAM-XIP address `0x80010000` on power-up/reset -- this is a
fact about the SES (Secure Enclave Services) configuration already on this
specific bench unit, not something this repo's build produces. Every step
below assumes that resident ATOC is already in place and unchanged.

## 2. Build both images, ITCM-retargeted

Both images are built to run out of ITCM (not MRAM-XIP) for this procedure,
so a debugger can place them directly at each core's local `0x00000000`
without going through the MRAM/ATOC path at all.

```sh
export ZEPHYR_BASE="$HOME/zephyrproject/zephyr"   # adjust to your west workspace
west build -p auto \
  -b e1m_aen/ae822fa0e5597ls0/rtss_hp \
  -d build/ae822fa0e5597ls0/rtss_hp-itcm \
  apps/dualcore_hp \
  -- \
  -DBOARD_ROOT=. \
  "-DZEPHYR_EXTRA_MODULES=$(pwd)/modules/alif-mhuv2;$(pwd)/modules/alif-se-boot" \
  -DDTC_OVERLAY_FILE=itcm.overlay

west build -p auto \
  -b e1m_aen/ae822fa0e5597ls0/rtss_he \
  -d build/ae822fa0e5597ls0/rtss_he-itcm \
  apps/dualcore_he \
  -- \
  -DBOARD_ROOT=. \
  -DZEPHYR_EXTRA_MODULES=$(pwd)/modules/alif-mhuv2 \
  -DDTC_OVERLAY_FILE=itcm.overlay
```

`itcm.overlay` content (identical for both apps):

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

## 3. The HE Flow C run (debugger loads and starts the resident-boot core)

This step is for the cluster the resident ATOC boots on its own (RTSS-HE on
this bench) -- "Flow C" here just names the load-then-settle-then-go sequence
used, distinct from Flow D (MRAM-XIP, mentioned in the board `.dts` comments)
and from ordinary `west flash`.

1. `loadbin <path-to-rtss_he-itcm-image>.bin 0x0` -- load the ITCM-retargeted
   HE image to its own local address 0.
2. `SetPC <entry>` -- set PC to the image's reset handler (read from the
   vector table / `.bin`'s word[1], per the same convention as step 5 below).
3. **`Sleep 20000`** -- a 20-second settle BEFORE `go`. **This is required,
   not a safety margin.** Measured across six runs: with no settle, console
   output is zero every time. The cause was not root-caused further within
   this bench session; treat the 20 s as load-bearing until it is.
4. `go` -- release the core to run.

## 4. The OpenOCD config that reaches the released HP core

RTSS-HP does not exist as a debuggable target until AFTER the SE release in
step 3 of the app's own boot sequence (`alif_se_boot_cpu(2, 0x50000000)`,
called from the running HE image) -- its Access Port only appears in the
DAP's AP list post-release, at `APAddr 0x00200000`. This OpenOCD config
reaches it once that has happened:

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
`pc`, `xPSR`, `CFSR`, `HFSR`) in step 5 below -- a plain `mem_ap` target only
gives raw memory access, not the CPU register set. `init` also auto-clears
the lockup state the core may be in immediately after release; no separate
unlock step is needed.

## 5. HP placement + register surgery (the debugger IS the boot mechanism here)

**The ordering warning, in bold, first: the SE release in step 3's
`alif_se_boot_cpu()` call WIPES the target core's TCM. An image written to
RTSS-HP's local `0x00000000` BEFORE that release is destroyed by it. The
image MUST be written AFTER `BOOT_CPU` (service_id 501) has already run,
never before.** This is why the HP image cannot simply be pre-loaded and
left resident the way the HE image is in step 3 -- the release itself erases
whatever was there.

Once the peer's `alif_se_boot_cpu(2, 0x50000000)` call (from the running HE
image, `apps/dualcore_hp`'s Kconfig-default release target) has returned
success and `chip.hp` is reachable per step 4:

1. `load_image <path-to-rtss_hp-itcm-image>.bin 0x00000000` -- write the
   ITCM-retargeted HP image to the core's own local address 0.
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
4. Write-1-to-clear the fault status registers, in case the release left the
   core in a fault state from the earlier VTOR==0 lockup class of failure:
   - `CFSR` (`0xE000ED28`) — write back its own current value
   - `HFSR` (`0xE000ED2C`) — write back its own current value
5. **Leave `VTOR` at its reset value, `0x00000000`.** Do NOT set it to the
   image's actual link address. The core fetches SP/PC from its own local
   `0x00000000` on this path regardless of `VTOR` -- see the note in
   `apps/dualcore_hp/Kconfig`'s `CONFIG_DEMO_RELEASE_PEER_ENTRY` help text on
   why the SE-reported entry address is decorative on this path.
6. `resume` -- release the core to run from the `pc`/`sp` just written.

## 6. Expected output on both sides

HE side (host, `apps/dualcore_hp` logic, running from the resident-boot
image placed per step 3):

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

HP side (remote, `apps/dualcore_he` logic, running from the image placed and
started by the debugger per step 5):

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
convention (`rtss_hp`/`rtss_he`), which -- per step 0 -- names which app
built the image, not necessarily which silicon cluster it ran on during this
particular bench session.

On the debug side, confirmation that the release reached the core: HP's
`NVIC_ISER1 = 0x00000800` (IRQ 43, the RPMsg doorbell, enabled) was read back
after `resume`.

## 7. Safety notes

- **Verify the SW-DP ID before any write.** This bench's probe must read
  `0x4C013477`. A read of `0x0BE12477` means the adapter is talking to a
  DIFFERENT board's cloned probe, reachable at USB path `3-4.2` on this
  bench's USB topology -- do not proceed with any write against that ID.
- **Never issue `SYSRESETREQ` through the HP AP.** The whole point of this
  procedure is that the HP core has no independent reset/boot path once
  released this way; a `SYSRESETREQ` here does not do what it would on a
  normal standalone target and has not been characterized.
- **Hold a labgrid reservation for the entire session.** Steps 3-6 above
  depend on state (the SE release, the AP's post-release appearance, the
  core's fault-register contents) that another user's concurrent session on
  the same bench would silently corrupt.

## 8. Bench-specific vs. what a customer carrier would do instead

Everything from step 3 onward (the debugger loading and starting the HP
core by hand) is **bench-specific** -- it exists because this bench has no
SETOOLS/ATOC entry for the RTSS-HP image and a debugger is standing in for
that mechanism session by session. A customer carrier would NOT do this in
production: it does not survive a power cycle and requires a debugger
permanently attached.

The customer-facing equivalent is a **two-entry ATOC** (built with Alif
SETOOLS' `app-gen-toc`) in which the RTSS-HP entry is flagged `["load"]` --
SES then places that image at its declared load address itself, as part of
normal boot, with no debugger involved. `alif_se_boot_cpu()`'s `entry_addr`
argument (`CONFIG_DEMO_RELEASE_PEER_ENTRY`) is the value that WOULD matter on
that path: it is the loadAddress the two-entry ATOC declares for the peer
image, and unlike the bench's debugger-placement path, the SES-ATOC path
genuinely uses it. This project has not built or exercised that ATOC path;
see `README.md` section 7 for the same caveat at the whole-project level.
