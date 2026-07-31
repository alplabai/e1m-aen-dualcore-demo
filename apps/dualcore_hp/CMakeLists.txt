#
# Copyright 2026 Alp Lab AB
#
# SPDX-License-Identifier: Apache-2.0
#
# dualcore_hp -- IPC-service RPMsg HOST demo for the rtss_hp cluster of the
# Alp Lab E1M-AEN SoM. Its counterpart, dualcore_he, is the RPMsg REMOTE and
# lives in ../dualcore_he. Build each independently and flash both to the
# board's split MRAM partitions (see boards/alp/e1m_aen for the layout).
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dualcore_hp)

target_sources(app PRIVATE src/main.c)
