# Reproducing the proven E1M-AEN801 dual-core RPMsg run

This document is the exact, ordered procedure that produced a live RPMsg
ping/pong link between the two Cortex-M55 clusters on E1M-AEN801 silicon on
**2026-07-30**: 369 consecutive `PONG seq=N rtt=32..35 us` lines, no gaps,
still running at `seq=563` when the session ended. This is the only
dual-core run this project has performed on real hardware to date.

**Read this before assuming the bench shape matches the repo's board-naming
convention.** It does not, exactly -- see step 0.

## 0. The bench shape is the MIRROR of what this repo's OLD `rtss_hp`/`rtss_he`-named apps assumed

At the time of the 2026-07-30 run described below, this repo's apps were
named `dualcore_hp`/`dualcore_he` and were written under the assumption that
the `rtss_hp` cluster is the one with Secure Enclave (SE) access and boots
first. **That assumption did not hold on the bench.** What was actually
observed:

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

**This has since been fixed.** The apps are now named by RPMsg role, not by
cluster -- `apps/dualcore_host` and `apps/dualcore_remote` -- and each builds
for both the `rtss_hp` and `rtss_he` board qualifiers (see each app's own
README). `apps/dualcore_host`'s `rtss_he` overlay now instantiates the same
`se_boot` devicetree node its `rtss_hp` overlay always did, so `west build
... -b e1m_aen/ae822fa0e5597ls0/rtss_he apps/dualcore_host` builds an image
that is directly flashable/placeable on the SES-booted cluster -- no
hand-written scratch host app needed anymore. Reproducing the run with THIS
repo's images now means:

- Build `apps/dualcore_host` for `e1m_aen/ae822fa0e5597ls0/rtss_he` (its
  Kconfig defaults, `CONFIG_DEMO_RELEASE_PEER_CPU_ID=2` /
  `CONFIG_DEMO_RELEASE_PEER_ENTRY=0x50000000`, already encode the
  bench-proven release parameters -- see `apps/dualcore_host/Kconfig` and
  `apps/dualcore_host/src/main.c`'s top-of-file comment).
- Build `apps/dualcore_remote` for `e1m_aen/ae822fa0e5597ls0/rtss_hp`.
- What IS reproducible today from this repo, unmodified: both `west build`
  commands in step 2 now succeed and produce correctly-sized images --
  `apps/dualcore_host` built for `rtss_he` links with `CONFIG_SRAM_SIZE=256`
  (matching RTSS-HE's real 256 KB DTCM) and its `se_boot` node is present,
  where previously only a hand-written scratch app not committed to this
  repo could do so. What is NOT independently re-confirmed by this fix: an
  actual silicon run of THIS lineage (new build -> debugger placement per
  steps 3-5) has not been repeated on the bench since the rename; the
  369-PONG count above was measured against the pre-rename scratch-app
  build. The two are expected to be functionally identical (same Kconfig
  defaults, same overlay content, same CONFIG_SRAM_SIZE), but that is an
  inference, not a fresh bench measurement -- see section 8 for what remains
  bench-specific either way.

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

`-DBOARD_ROOT` must be an ABSOLUTE path -- `-DBOARD_ROOT=.` fails with
`Invalid BOARD; see above.` because CMake evaluates it relative to the build
directory, not the shell's current directory, so a bare `.` does not resolve
to this repo's root the way it looks like it should.

`-DDTC_OVERLAY_FILE=itcm.overlay` ALONE **replaces** the board's
auto-picked overlay list instead of adding to it, silently dropping the
app's own board overlay (the one that instantiates `ipc0`, `mhu_tx`/`mhu_rx`,
and -- for the host build -- `se_boot`). The build still configures, but
fails later with `'__device_dts_ord_DT_N_NODELABEL_ipc0_ORD' undeclared`.
Pass BOTH files, semicolon-separated, so the auto-picked overlay is still
applied alongside the ITCM retarget:

```sh
export ZEPHYR_BASE="$HOME/zephyrproject/zephyr"   # adjust to your west workspace
REPO="$(pwd)"   # this repo's absolute path -- BOARD_ROOT below must be absolute

west build -p auto \
  -b e1m_aen/ae822fa0e5597ls0/rtss_he \
  -d build/ae822fa0e5597ls0/rtss_he-itcm \
  apps/dualcore_host \
  -- \
  "-DBOARD_ROOT=${REPO}" \
  "-DZEPHYR_EXTRA_MODULES=${REPO}/modules/alif-mhuv2;${REPO}/modules/alif-se-boot" \
  "-DDTC_OVERLAY_FILE=boards/e1m_aen_ae822fa0e5597ls0_rtss_he.overlay;itcm.overlay"

west build -p auto \
  -b e1m_aen/ae822fa0e5597ls0/rtss_hp \
  -d build/ae822fa0e5597ls0/rtss_hp-itcm \
  apps/dualcore_remote \
  -- \
  "-DBOARD_ROOT=${REPO}" \
  "-DZEPHYR_EXTRA_MODULES=${REPO}/modules/alif-mhuv2" \
  "-DDTC_OVERLAY_FILE=boards/e1m_aen_ae822fa0e5597ls0_rtss_hp.overlay;itcm.overlay"
```

`itcm.overlay` (identical for both apps, and now committed in-tree at
`apps/dualcore_host/itcm.overlay` / `apps/dualcore_remote/itcm.overlay` --
earlier drafts of this document only showed its contents without the file
actually existing in the repo):

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

1. `loadbin build/ae822fa0e5597ls0/rtss_he-itcm/zephyr/zephyr.bin 0x0` --
   load the ITCM-retargeted HOST image (built for `rtss_he` per section 2)
   to its own local address 0.
2. `SetPC <entry>` -- set PC to the image's reset handler, read from the
   vector table / `.bin`'s word[1] WITH BIT 0 CLEARED (e.g. `0x265C`, not
   `0x265D`) -- the same reset-vector convention used in step 5 below.
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

Once the peer's `alif_se_boot_cpu(2, 0x50000000)` call (from the running HOST
image, `apps/dualcore_host`'s Kconfig-default release target) has returned
success and `chip.hp` is reachable per step 4:

1. `load_image build/ae822fa0e5597ls0/rtss_hp-itcm/zephyr/zephyr.bin
   0x00000000` -- write the ITCM-retargeted REMOTE image (built for
   `rtss_hp` per section 2) to the core's own local address 0.
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
   `apps/dualcore_host/Kconfig`'s `CONFIG_DEMO_RELEASE_PEER_ENTRY` help text
   on why the SE-reported entry address is decorative on this path.
6. `resume` -- release the core to run from the `pc`/`sp` just written.

## 6. Expected output on both sides

HE side (host, `apps/dualcore_host` logic, running from the resident-boot
image placed per step 3). This is the only side actually captured on this
bench -- see the note below:

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

HP side (remote, `apps/dualcore_remote` logic, running from the image placed
and started by the debugger per step 5). Expected content shown for
completeness, but see the note below for why this side's console could not
actually be captured on this bench:

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
convention (`rtss_hp`/`rtss_he`), which names which app built the image, not
necessarily which silicon cluster it ran on during this particular bench
session -- see step 0.

**The HP-side (remote) transcript above is NOT actually capturable on this
bench.** The remote app's console is `uart3`, but only UART5 is physically
routed to this bench's `/dev/ttyUSB2`. The HOST-side PONG count (369
consecutive, no gaps) is the confirmation that the remote side was in fact
running and echoing correctly -- every PONG implies a prior successful PING
round trip -- but no direct transcript of the remote's own log lines exists
from this bench session.

On the debug side, confirmation that the release reached the core: HP's
`NVIC_ISER1 = 0x00000800` (IRQ 43, the RPMsg doorbell, enabled) was read back
after `resume`.

## 7. Safety notes

- **Verify the SW-DP ID before any write.** This bench's probe must read
  `0x4C013477`. A read of `0x0BE12477` means the adapter is talking to a
  DIFFERENT board's cloned probe, reachable at USB path `3-4.2` on this
  bench's USB topology -- do not proceed with any write against that ID.
  A DP-only check that never touches the target -- just section 4's OpenOCD
  config with its `target create chip.hp ...` line removed -- gives the
  SWD DPIDR with no failed AP examination to wade through:

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
  init
  ```

  Expect `DPIDR 0x4c013477` and no `Examination failed` lines -- with
  `target create` present, OpenOCD also tries to examine the AP, which
  fails (as expected) before RTSS-HP is released and adds noise to a check
  that is only about the DP.
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
