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
# THIS SCRIPT HAS NOT BEEN RUN ON SILICON. Its staging/config directory
# layout (SETOOLS_DIR/build/images, SETOOLS_DIR/build/config) follows the
# convention named in modules/alif-se-boot/include/alif_se_boot.h's
# alif_se_process_toc_entry() doc comment ("Alif SETOOLS'
# app-release-exec-linux/build/config directory"); adjust
# IMAGES_SUBDIR/CONFIG_SUBDIR below if your SETOOLS layout differs.
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

echo "==> Done. This has NOT been verified on silicon by this script --" \
     "confirm the reset vector / boot behaviour on your own bench before" \
     "relying on it."
