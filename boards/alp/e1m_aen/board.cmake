# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0

# AE822FA0E5597LS0 (E1M-AEN801, Alif Ensemble E8 -- the silicon actually on hand).
# Device strings mirror upstream zephyr/boards/alif/ensemble_e8_dk/board.cmake,
# where SoC name and J-Link --device= string agree (both LS0).
if(CONFIG_SOC_AE822FA0E5597LS0_RTSS_HP)
  board_runner_args(jlink "--device=AE822FA0E5597LS0_M55_HP" "--speed=4000")
endif()

if(CONFIG_SOC_AE822FA0E5597LS0_RTSS_HE)
  board_runner_args(jlink "--device=AE822FA0E5597LS0_M55_HE" "--speed=4000")
endif()

# AE402FA0E5597LE0 (E1M-AEN401, Alif Ensemble E4 -- PRIMARY target).
#
# *** UNVERIFIED J-LINK DEVICE NAME ***
# Upstream zephyr/boards/alif/ensemble_e8_dk/board.cmake maps this exact SoC
# (CONFIG_SOC_AE402FA0E5597LE0_RTSS_{HP,HE}) to device strings "AE402FA0E5597LS0_M55_*"
# -- note LS0, not LE0. That looks like a copy-paste bug in the upstream file (the
# SoC is unambiguously ...LE0 per its own dtsi/Kconfig), but we do NOT know whether
# Segger's JLinkDevices XML actually ships an "AE402FA0E5597LE0_M55_*" entry, or only
# the LS0 one, or something else entirely. We deliberately use the LE0-consistent
# string below rather than silently copying upstream's LS0 string. Before the first
# flash of an LE0 target, run `JLinkExe -SelectEmuBySN <sn>` -> `?` (device list) or
# check Segger's online device database for "AE402FA0E5597LE0" and correct this line
# if it does not exist verbatim.
if(CONFIG_SOC_AE402FA0E5597LE0_RTSS_HP)
  board_runner_args(jlink "--device=AE402FA0E5597LE0_M55_HP" "--speed=4000")
endif()

if(CONFIG_SOC_AE402FA0E5597LE0_RTSS_HE)
  board_runner_args(jlink "--device=AE402FA0E5597LE0_M55_HE" "--speed=4000")
endif()

include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
