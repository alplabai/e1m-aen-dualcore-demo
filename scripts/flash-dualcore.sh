#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# Stage both dual-core images and this repo's committed ATOC config into an
# Alif Security Toolkit (SETOOLS) checkout, then generate and write the ATOC
# to MRAM over the SE-UART console.
#
# WHAT THIS DOES NOT DO: it does not build the images (see
# `scripts/build-all.sh` / README.md section 6 for the HOST build and
# docs/BENCH-DUALCORE.md section 3.1 for the REMOTE ITCM build), and it does
# not install or configure SETOOLS itself -- SETOOLS is licence-gated and is
# NOT redistributed in this repository. See atoc/README.md for the full
# entry-to-build mapping this script assumes.
#
# THIS SCRIPT HAS BEEN RUN ON SILICON: 2026-08-04, E1M-AEN801
# (AE822FA0E5597LS0 Rev A0), against the committed
# atoc/e1m-aen801-dualcore.json with no by-hand edits -- see
# docs/BENCH-DUALCORE.md section 2.7 for the full measured record (SES
# boot table, REMOTE console transcript, MRAM persistence across a cold
# power cycle). That result is scoped to that part, that date, and that
# bench -- it is not a blanket guarantee for every SoM/carrier this repo
# also names. Its staging/config directory layout (SETOOLS_DIR/build/images,
# SETOOLS_DIR/build/config) follows the convention named in
# modules/alif-se-boot/include/alif_se_boot.h's alif_se_process_toc_entry()
# doc comment ("Alif SETOOLS' app-release-exec-linux/build/config
# directory"); adjust IMAGES_SUBDIR/CONFIG_SUBDIR below if your SETOOLS
# layout differs.
#
# Usage:
#   SETOOLS_DIR=/path/to/app-release-exec-linux \
#   SE_UART=/dev/ttyUSB0 \
#   scripts/flash-dualcore.sh [host-build-dir] [remote-itcm-build-dir]
#
#   host-build-dir          optional; defaults to
#                            build/ae822fa0e5597ls0/rtss_he (the default MRAM
#                            HOST build -- README.md section 6)
#   remote-itcm-build-dir   optional; defaults to
#                            build/ae822fa0e5597ls0/rtss_hp-itcm (the REMOTE
#                            build with -DDTC_OVERLAY_FILE=boards/e1m_aen_
#                            ae822fa0e5597ls0_rtss_hp.overlay;itcm.overlay --
#                            docs/BENCH-DUALCORE.md section 3.1)
#
# Required environment (never hardcoded -- this script errors clearly if
# either is unset):
#   SETOOLS_DIR   absolute path to the Alif Security Toolkit checkout
#                 (contains app-gen-toc / app-write-mram).
#   SE_UART       the SE-UART device app-write-mram should use, e.g.
#                 /dev/ttyUSB0 or /dev/ttyACM0.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd -P)"
PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." >/dev/null 2>&1 && pwd -P)"

usage() {
	cat >&2 <<'EOF'
Usage:
  SETOOLS_DIR=/path/to/app-release-exec-linux \
  SE_UART=/dev/ttyUSB0 \
  scripts/flash-dualcore.sh [host-build-dir] [remote-itcm-build-dir]

Required environment:
  SETOOLS_DIR   absolute path to the Alif Security Toolkit checkout
  SE_UART       SE-UART device for app-write-mram, e.g. /dev/ttyUSB0

Optional positional arguments:
  host-build-dir          default: build/ae822fa0e5597ls0/rtss_he
  remote-itcm-build-dir   default: build/ae822fa0e5597ls0/rtss_hp-itcm
EOF
}

if [ -z "${SETOOLS_DIR:-}" ]; then
	echo "error: SETOOLS_DIR is not set -- point it at your Alif Security" \
	     "Toolkit checkout (the tool is licence-gated and is NOT" \
	     "redistributed in this repository; see atoc/README.md)." >&2
	usage
	exit 1
fi

if [ -z "${SE_UART:-}" ]; then
	echo "error: SE_UART is not set -- point it at the SE-UART device," \
	     "e.g. SE_UART=/dev/ttyUSB0." >&2
	usage
	exit 1
fi

if [ ! -d "${SETOOLS_DIR}" ]; then
	echo "error: SETOOLS_DIR '${SETOOLS_DIR}' is not a directory." >&2
	exit 1
fi

# A directory alone doesn't mean a real SETOOLS checkout -- catch a
# pointed-at-the-wrong-place SETOOLS_DIR here, before staging anything,
# rather than after app-gen-toc/app-write-mram fail with a bare
# "command not found". Windows SETOOLS distributions carry a .exe suffix;
# accept either.
for tool in app-gen-toc app-write-mram; do
	if [ ! -e "${SETOOLS_DIR}/${tool}" ] && [ ! -e "${SETOOLS_DIR}/${tool}.exe" ]; then
		echo "error: '${tool}' not found in SETOOLS_DIR '${SETOOLS_DIR}' --" \
		     "is this really an Alif Security Toolkit checkout" \
		     "(app-release-exec-linux or equivalent)?" >&2
		exit 1
	fi
done

HOST_BUILD_DIR="${1:-${PROJECT_ROOT}/build/ae822fa0e5597ls0/rtss_he}"
REMOTE_BUILD_DIR="${2:-${PROJECT_ROOT}/build/ae822fa0e5597ls0/rtss_hp-itcm}"

HOST_BIN="${HOST_BUILD_DIR}/zephyr/zephyr.bin"
REMOTE_BIN="${REMOTE_BUILD_DIR}/zephyr/zephyr.bin"
ATOC_SRC="${PROJECT_ROOT}/atoc/e1m-aen801-dualcore.json"

for f in "${HOST_BIN}" "${REMOTE_BIN}" "${ATOC_SRC}"; do
	if [ ! -f "${f}" ]; then
		echo "error: expected file not found: ${f}" >&2
		exit 1
	fi
done

# SETOOLS' own convention (see this file's top-of-file comment): app-gen-toc
# resolves each entry's "binary" field against an images directory, and reads
# the ATOC JSON itself from a config directory.
IMAGES_SUBDIR="build/images"
CONFIG_SUBDIR="build/config"

IMAGES_DIR="${SETOOLS_DIR}/${IMAGES_SUBDIR}"
CONFIG_DIR="${SETOOLS_DIR}/${CONFIG_SUBDIR}"

mkdir -p "${IMAGES_DIR}" "${CONFIG_DIR}"

# --- Link-address guard --------------------------------------------------
# word[1] of a Cortex-M .bin (byte offset 4, 4 bytes, little-endian) is the
# reset handler address -- see docs/BENCH-DUALCORE.md section 3.4's
# "Cortex-M reset convention" note. Catching a swapped or stale build here
# is one failed `west build` re-run; catching it after app-write-mram has
# already written it is a wasted bench cycle -- see atoc/README.md's
# "Which builds feed which entry" table for why the two ATOC entries need
# DIFFERENTLY-BUILT images, not just differently-named ones.
#
# od -A n -t u1 (address radix "none", one unsigned decimal byte per
# field) is used instead of `od --endian=little` because the latter is a
# GNU extension: macOS/BSD od has no --endian flag at all, and this
# script's portability bar (see scripts/build-all.sh's own bash-3.2 note)
# assumes a bench operator may be on either.
read_word_le() {
	# $1 = file, $2 = byte offset. Reassigned below to the four byte values
	# themselves once od has read them -- the file/offset args are only
	# needed to build the od invocation.
	local file byte_offset
	file="$1"
	byte_offset="$2"
	# shellcheck disable=SC2046 # word-splitting od's output is intentional
	set -- $(od -A n -t u1 -j "${byte_offset}" -N 4 "${file}")
	printf '%d\n' "$(( $1 + ($2 * 256) + ($3 * 65536) + ($4 * 16777216) ))"
}

HOST_WORD1="$(read_word_le "${HOST_BIN}" 4)"
REMOTE_WORD1="$(read_word_le "${REMOTE_BIN}" 4)"

# HOST must be linked to run XIP from the resident ATOC's ALP-HE boot
# address, 0x80010000 (README.md section 5 / atoc/e1m-aen801-dualcore.json)
# -- i.e. word[1]'s top 16 bits must read 0x8001. Measured on this repo's
# own bench-proven HOST build (2026-08-04): word[1] = 0x8001264D.
if [ "$(( HOST_WORD1 & 0xFFFF0000 ))" -ne "$(( 0x80010000 ))" ]; then
	printf 'error: %s does not look like the HOST (ALP-HE) build.\n' "${HOST_BIN}" >&2
	printf '  word[1] (reset handler) = 0x%08X, expected 0x8001xxxx.\n' "${HOST_WORD1}" >&2
	printf '  This must be the DEFAULT MRAM build of apps/dualcore_host for\n' >&2
	printf '  rtss_he -- see README.md section 6 and its section 5 slot map.\n' >&2
	exit 1
fi

# REMOTE must NOT be MRAM-linked -- the committed ATOC's ALP-HP entry is a
# loadAddress (ITCM) entry, not an mramAddress (XIP) one (atoc/README.md's
# "Which builds feed which entry" table), so this must be the ITCM build
# (docs/BENCH-DUALCORE.md section 2.3), which links at the M55-HP ITCM
# local address (word[1] = 0x000025CD on this repo's own bench-proven
# build), never at an MRAM address (0x80xxxxxx -- MRAM base is
# 0x80000000, so a top byte of 0x80 means MRAM-linked regardless of
# offset).
if [ "$(( REMOTE_WORD1 & 0xFF000000 ))" -eq "$(( 0x80000000 ))" ]; then
	printf 'error: %s is MRAM-linked (word[1] = 0x%08X).\n' "${REMOTE_BIN}" "${REMOTE_WORD1}" >&2
	printf '  The committed ATOC'"'"'s ALP-HP entry is a loadAddress (ITCM) entry --\n' >&2
	printf '  it needs the ITCM REMOTE build, not the default MRAM one. See\n' >&2
	printf '  docs/BENCH-DUALCORE.md section 2.3 and atoc/README.md'"'"'s "Which\n' >&2
	printf '  builds feed which entry" table.\n' >&2
	exit 1
fi

# The DEVICE entry's binary (atoc/e1m-aen801-dualcore.json's "DEVICE"."binary")
# is supplied by the Alif Security Toolkit itself, not by this repository --
# see atoc/README.md's licence-gating note. app-gen-toc resolves it against
# the SAME images directory as the two app binaries above (see the "Names
# must match" comment below), so check for it here too rather than letting
# app-gen-toc fail deeper into the toolchain with a less specific error.
# This is a pre-flight NOTE, not a hard requirement this script can fully
# verify -- this repo does not know your SETOOLS checkout's exact layout
# or which signing material (keys/certificates) app-gen-toc needs for the
# "signed": true entries; consult your Alif SETOOLS documentation for both.
if [ ! -f "${IMAGES_DIR}/app-device-config.json" ]; then
	echo "note: app-device-config.json was not found yet in the images" \
	     "staging directory -- app-gen-toc needs it for the DEVICE entry." \
	     "It is supplied by the Alif Security Toolkit, not by this repo" \
	     "(see atoc/README.md); copy it in before running this script if" \
	     "app-gen-toc hasn't already been pointed at it another way." >&2
fi

# Names must match atoc/e1m-aen801-dualcore.json's "binary" fields exactly --
# ALP-HE -> dualcore_host.bin (default MRAM HOST build), ALP-HP ->
# dualcore_remote.bin (ITCM REMOTE build). See atoc/README.md section "Which
# builds feed which entry" for why swapping these two wastes a bench cycle.
cp -f "${HOST_BIN}" "${IMAGES_DIR}/dualcore_host.bin"
cp -f "${REMOTE_BIN}" "${IMAGES_DIR}/dualcore_remote.bin"

ATOC_DST="${CONFIG_DIR}/e1m-aen801-dualcore.json"
cp -f "${ATOC_SRC}" "${ATOC_DST}"

echo "==> Staged HOST image:   ${IMAGES_DIR}/dualcore_host.bin"
echo "==> Staged REMOTE image: ${IMAGES_DIR}/dualcore_remote.bin"
echo "==> Staged ATOC config:  ${ATOC_DST}"

pushd "${SETOOLS_DIR}" >/dev/null

echo "==> app-gen-toc -f ${CONFIG_SUBDIR}/e1m-aen801-dualcore.json"
./app-gen-toc -f "${CONFIG_SUBDIR}/e1m-aen801-dualcore.json"

echo "==> app-write-mram -c \"${SE_UART}\" -p"
./app-write-mram -c "${SE_UART}" -p

popd >/dev/null

echo "==> Done. This exact flow (this script, this repo's committed ATOC," \
     "no by-hand edits) was measured working on E1M-AEN801" \
     "(AE822FA0E5597LS0 Rev A0) on 2026-08-04 -- see" \
     "docs/BENCH-DUALCORE.md section 2.7. That result is scoped to that" \
     "part/date/bench, not a guarantee for yours."
echo "==> This script does NOT read anything back after the write. A" \
     "reported-success exit code is not by itself proof of a successful" \
     "release/boot -- this repo's own bench record shows SE service calls" \
     "reporting success without the intended effect being independently" \
     "confirmed (docs/BENCH-DUALCORE.md sections 2.5 and 3.7); treat" \
     "app-write-mram's exit status the same way. At minimum, read back" \
     "MRAM 0x80010000's first four words over SWD/JTAG, or the SES boot" \
     "table over the SE-UART, and compare against the HOST .bin staged" \
     "above before trusting this write -- see" \
     "docs/BENCH-DUALCORE.md section 2.7 for what that comparison looked" \
     "like on this repo's own 2026-08-04 bench run."
