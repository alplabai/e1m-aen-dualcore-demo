# SPDX-FileCopyrightText: Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# rgb_fade -- fade the E1M-AEN carrier's RGB LED via the Alif UTIMER PWM
# driver, using only the standard Zephyr PWM API.
#
# Unlike the dual-core apps in this repo, this one must be built against
# ALIF'S Zephyr fork (alifsemi/zephyr_alif, fetched by the alifsemi/sdk-alif
# west manifest), because upstream Zephyr has no Alif PWM driver, no UTIMER
# devicetree nodes and no UTIMER clock id. See ../../docs/PWM-RGB-PORT.md.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rgb_fade LANGUAGES C)

target_sources(app PRIVATE src/main.c)
