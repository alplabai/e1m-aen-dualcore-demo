#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# Build every e1m_aen dual-core target: the HOST app (apps/dualcore_host)
# against one qualifier and the REMOTE app (apps/dualcore_remote) against the
# other, per SoC.
#
# ROLE -> CORE MAPPING (this is the whole point of this script's
# core_app_dir(), see below): on the E1M-AEN801 bench unit
# (ae822fa0e5597ls0), the resident ATOC boots M55-HE first and only that
# cluster has Secure Enclave access, so HOST must run on rtss_he and REMOTE
# on rtss_hp -- see docs/BENCH-DUALCORE.md section 0. That is this script's
# DEFAULT for BOTH SoCs. The E1M-AEN401 (ae402fa0e5597le0) mapping has never
# been observed on real silicon -- applying the same default to it is a
# simplifying assumption, not a bench-confirmed fact; do not treat an E4
# build result as validating its boot order.
#
# Usage:
#   scripts/build-all.sh [ae402fa0e5597le0 | ae822fa0e5597ls0 | all]
#
#   No argument           -> ae402fa0e5597le0 only (E1M-AEN401, the primary
#                             target), 2 targets: host->rtss_he, remote->rtss_hp.
#   ae822fa0e5597ls0      -> E1M-AEN801 only (the silicon on hand, bench-proven
#                             mapping), 2 targets: host->rtss_he, remote->rtss_hp.
#   all                   -> both SoCs, all four board targets:
#                               e1m_aen/ae402fa0e5597le0/rtss_he  (host)
#                               e1m_aen/ae402fa0e5597le0/rtss_hp  (remote)
#                               e1m_aen/ae822fa0e5597ls0/rtss_he  (host, bench-proven)
#                               e1m_aen/ae822fa0e5597ls0/rtss_hp  (remote, bench-proven)
#
# This script always builds the DEFAULT mapping above. The other mapping
# (host->rtss_hp / remote->rtss_he) is still buildable -- both apps carry
# overlays for both qualifiers -- but only via a direct `west build`
# invocation naming the app and qualifier explicitly; this script does not
# have a flag for it, to keep the common path a single unambiguous default
# rather than a silently-branching one.
#
# Required environment:
#   ZEPHYR_BASE  must already point at a v4.4.0 Zephyr checkout (this script
#                does not run "west update" for you -- see README.md).
#   WEST         optional override for the west executable; defaults to
#                "west" resolved from $PATH so this script carries no
#                hardcoded path into anyone's home directory.

set -euo pipefail

# Resolve the project root from the script's own location (not a hardcoded
# path) so this works from any checkout / any user's home directory.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd -P)"
PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." >/dev/null 2>&1 && pwd -P)"

WEST="${WEST:-west}"
if ! command -v "${WEST}" >/dev/null 2>&1; then
	echo "error: '${WEST}' not found on PATH; set WEST=/path/to/west or activate your Zephyr venv" >&2
	exit 1
fi

# ZEPHYR_BASE is honoured, never assumed: west's CMake package lookup for
# find_package(Zephyr) falls back to this env var when no west workspace
# topdir is found above the app directory (see the README's build-topology
# note for why this repo can be built either as its own west workspace or
# pointed at an existing zephyrproject checkout).
: "${ZEPHYR_BASE:?ZEPHYR_BASE must be set to a Zephyr v4.4.0 checkout, e.g. export ZEPHYR_BASE=/path/to/zephyrproject/zephyr}"

BOARD_ROOT="${PROJECT_ROOT}"
# Both modules are passed for every target (host and remote alike):
# alif-se-boot's CONFIG_ALIF_SE_BOOT only turns on where its DT node is
# instantiated (dualcore_host's overlays, both qualifiers), so listing it
# here has no effect on the dualcore_remote build -- see
# modules/alif-se-boot/README.md.
ZEPHYR_EXTRA_MODULES="${PROJECT_ROOT}/modules/alif-mhuv2;${PROJECT_ROOT}/modules/alif-se-boot"

SOC_ARG="${1:-ae402fa0e5597le0}"

case "${SOC_ARG}" in
ae402fa0e5597le0 | ae822fa0e5597ls0)
	SOCS=("${SOC_ARG}")
	;;
all)
	SOCS=("ae402fa0e5597le0" "ae822fa0e5597ls0")
	;;
*)
	echo "error: unknown SoC '${SOC_ARG}' (expected ae402fa0e5597le0, ae822fa0e5597ls0, or all)" >&2
	exit 1
	;;
esac

# core -> (app directory, role) mapping. THIS is the bench-proven default
# from this file's top-of-file comment: host runs on rtss_he, remote on
# rtss_hp. Applied uniformly to both SoCs -- see the caveat above about the
# E4 (ae402fa0e5597le0) mapping being unconfirmed.
# NOTE: deliberately NOT a `declare -A` associative array. macOS ships GNU bash
# 3.2.57 (the last GPLv2 release), which has no associative arrays -- `declare -A`
# there fails at RUN time, not parse time, so `bash -n` passes and the script then
# dies with "rtss_hp: unbound variable" before building anything. A case statement
# is portable back to bash 3.x.
core_app_dir() {
	case "$1" in
	rtss_hp) printf '%s\n' "${PROJECT_ROOT}/apps/dualcore_remote" ;;
	rtss_he) printf '%s\n' "${PROJECT_ROOT}/apps/dualcore_host" ;;
	*) echo "error: unknown core '$1'" >&2; return 1 ;;
	esac
}

core_role() {
	case "$1" in
	rtss_hp) printf '%s\n' "remote" ;;
	rtss_he) printf '%s\n' "host" ;;
	*) echo "error: unknown core '$1'" >&2; return 1 ;;
	esac
}

# Results accumulated as "target|result" pairs for the closing summary table.
RESULTS=()
OVERALL_STATUS=0

for soc in "${SOCS[@]}"; do
	for core in rtss_hp rtss_he; do
		board="e1m_aen/${soc}/${core}"
		app_dir="$(core_app_dir "${core}")"
		role="$(core_role "${core}")"
		build_dir="${PROJECT_ROOT}/build/${soc}/${core}"

		echo "==> Building ${board} as ${role} (app: ${app_dir}) -- see this" \
		     "script's top-of-file comment for why ${core} is ${role} on this SoC"

		if "${WEST}" build \
			-p auto \
			-b "${board}" \
			-d "${build_dir}" \
			"${app_dir}" \
			-- \
			"-DBOARD_ROOT=${BOARD_ROOT}" \
			"-DZEPHYR_EXTRA_MODULES=${ZEPHYR_EXTRA_MODULES}"; then
			RESULTS+=("${board} (${role})|PASS")
		else
			RESULTS+=("${board} (${role})|FAIL")
			OVERALL_STATUS=1
		fi
	done
done

echo
echo "==================== build-all summary ===================="
printf "%-38s %s\n" "target" "result"
printf "%-38s %s\n" "------" "------"
for entry in "${RESULTS[@]}"; do
	IFS='|' read -r target result <<<"${entry}"
	printf "%-38s %s\n" "${target}" "${result}"
done
echo "=============================================================="

exit "${OVERALL_STATUS}"
