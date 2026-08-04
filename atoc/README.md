# ATOC config for the E1M-AEN801 dual-core demo

This directory carries the ATOC (Application Table of Contents) config this
repo's dual-core demo is built to work with. **It HAS been run on silicon
in this exact committed form** -- measured 2026-08-04 on E1M-AEN801
(`AE822FA0E5597LS0` Rev A0) via `scripts/flash-dualcore.sh`, with no
by-hand edits to this file. See the root `README.md` and
`docs/BENCH-DUALCORE.md` section 2.7 for the full measured record and their
caveats (in particular, section 2.5's separate, still-open disagreement
over whether the deferred `ALP-HP` entry alone always suffices).

## `e1m-aen801-dualcore.json`

```json
{
    "DEVICE":  { "disabled": false, "binary": "app-device-config.json", "version": "0.5.00", "signed": true },
    "ALP-HE":  { "disabled": false, "binary": "dualcore_host.bin",   "version": "1.0.0", "signed": true,
                 "cpu_id": "M55_HE", "mramAddress": "0x80010000", "flags": ["boot"] },
    "ALP-HP":  { "disabled": false, "binary": "dualcore_remote.bin", "version": "1.0.0", "signed": true,
                 "cpu_id": "M55_HP", "loadAddress": "0x50000000", "flags": ["load", "boot", "deferred"] }
}
```

Three entries, three different jobs:

- **`DEVICE`** -- the device-configuration entry every Alif ATOC needs.
  Its `binary`, `app-device-config.json`, is **supplied by the Alif
  Security Toolkit** (SETOOLS) itself, not by this repository -- see
  "SETOOLS is licence-gated" below.
- **`ALP-HE`** -- the **HOST** role (`apps/dualcore_host`), which this
  SoM's resident ATOC boots directly at MRAM-XIP address `0x80010000` (see
  `docs/BENCH-DUALCORE.md` section 1). `"flags": ["boot"]`, no
  `"deferred"` -- SES boots this entry at cold boot with no runtime
  un-defer call needed.
- **`ALP-HP`** -- the **REMOTE** role (`apps/dualcore_remote`), the peer
  the HOST releases at runtime. `"loadAddress": "0x50000000"` (the M55-HP
  ITCM global alias) marks this an ITCM-load entry, not an MRAM-XIP one --
  see "Which builds feed which entry" below. Its flags carry `"deferred"`,
  which is what makes it eligible for the runtime un-defer call
  `apps/dualcore_host/src/main.c` issues.

### `"deferred"` is a flags-array MEMBER, not a sibling key

`"flags": ["load", "boot", "deferred"]` puts `"deferred"` alongside
`"load"`/`"boot"` **inside the array**. A sibling `"deferred": true` key on
the entry object (e.g. `{ "flags": ["load", "boot"], "deferred": true }`) is
a **different, non-equivalent encoding** -- the Alif ATOC builder
(`app-gen-toc`) rejects it. Only the array-member form sets
`TOC_IMAGE_DEFERRED = 0x100` in the entry's on-the-wire flags word; see
`modules/alif-se-boot/include/alif_se_boot.h`'s
`alif_se_process_toc_entry()` doc comment for the bench observation this was
confirmed against (`0x00000022` -> `0x00000122`).

### The entry name `ALP-HP` must match `DEMO_RELEASE_TOC_ENTRY_ID`

`apps/dualcore_host/src/main.c` calls
`alif_se_process_toc_entry(DEMO_RELEASE_TOC_ENTRY_ID)` to un-defer the peer
at runtime, where `DEMO_RELEASE_TOC_ENTRY_ID` is `#define`d as `"ALP-HP"`.
This JSON's peer entry is named `"ALP-HP"` to match that literal exactly --
if either side is ever renamed, the other MUST be updated in lockstep, or
the runtime un-defer call names an ATOC entry that does not exist. (Grep
`src/main.c` for `DEMO_RELEASE_TOC_ENTRY_ID` before touching either side.)

## Which builds feed which entry

Getting this pair backwards is the easiest way to waste a bench cycle --
each entry needs a DIFFERENTLY-BUILT image, not just a differently-named
one:

| ATOC entry | Feeds from | Why |
|---|---|---|
| `ALP-HE` | The **default MRAM** HOST build, `build/ae822fa0e5597ls0/rtss_he` (`apps/dualcore_host`, `rtss_he` qualifier, no ITCM overlay) -- now linking at `0x80010000` after the slot-map fix in `boards/alp/e1m_aen/e1m_aen_ae822fa0e5597ls0_rtss_he.dts` | `mramAddress` in the ATOC entry means SES boots this image XIP, directly out of MRAM, at the address given -- it must actually be linked (and flashed) there. |
| `ALP-HP` | The **ITCM** REMOTE build, built with `-DDTC_OVERLAY_FILE=boards/e1m_aen_ae822fa0e5597ls0_rtss_hp.overlay;itcm.overlay` (see `docs/BENCH-DUALCORE.md` section 3.1) -- e.g. `build/ae822fa0e5597ls0/rtss_hp-itcm` | `loadAddress` in the ATOC entry means SES LOADS this image into the address given (the M55-HP ITCM global alias, `0x50000000`) rather than executing it XIP from MRAM -- an image linked for MRAM-XIP would not run correctly if loaded into ITCM this way. A `loadAddress` key is what makes an entry an ITCM-load entry as opposed to an `mramAddress` (XIP) entry. |

`scripts/flash-dualcore.sh` stages exactly these two build outputs under the
filenames this JSON's `binary` fields reference (`dualcore_host.bin`,
`dualcore_remote.bin`).

## Flashing order: ATOC last

The ATOC must be written to MRAM **after both images it references already
exist in MRAM/are staged for `app-write-mram`** -- the ATOC entries point at
binaries by name; writing the ATOC before both images are in place gives
`app-gen-toc`/`app-write-mram` nothing to resolve those references against.
This applies identically to the deferred `ALP-HP` entry: `"deferred"`
changes WHEN SES processes the entry (at the runtime
`alif_se_process_toc_entry()` call instead of at cold boot), not whether the
image needs to already be present first. See `docs/BENCH-DUALCORE.md`
section 2.4 and root `README.md` section 7.

## A "NOT multiple of 16 bytes" warning from `app-write-mram` is expected, not an error

`app-write-mram` pads any staged binary that is not a multiple of 16 bytes
(MRAM's write granularity) up to the next multiple when run with `-p`,
printing a warning naming the file and its size while it does so -- e.g.
`the SIZE of .../dualcore_host.bin is NOT multiple of 16 bytes as required
by MRAM` (observed 2026-08-04, `dualcore_host.bin` padded by 4 bytes). This
is routine housekeeping, not a sign the write failed or the image is
corrupt -- do not treat it as a fatal error or try to pre-pad the `.bin`
yourself to silence it.

## SETOOLS is licence-gated -- not redistributed here

The Alif Security Toolkit (`app-gen-toc`, `app-write-mram`, and the
`app-device-config.json` this ATOC's `DEVICE` entry references) is
distributed by Alif Semiconductor under its own licence terms. **It is NOT
included in, generated by, or redistributed from this repository.** Obtain
it directly from Alif and supply its own `app-device-config.json` for the
`DEVICE` entry above -- this repo commits only the ATOC JSON that names the
two dual-core images it builds.
