#
# Copyright 2026 Alp Lab AB
#
# SPDX-License-Identifier: Apache-2.0
#
# dualcore_remote -- IPC-service RPMsg REMOTE demo. Its role runs on
# whichever M55 cluster is NOT the resident ATOC's boot-first cluster (see
# docs/BENCH-DUALCORE.md section 0) -- this app builds for both the rtss_hp
# and rtss_he board qualifiers, see boards/*.overlay. Its counterpart,
# dualcore_host, is the RPMsg HOST and lives in ../dualcore_host. Build each
# independently and flash both to the board's split MRAM partitions (see
# boards/alp/e1m_aen for the layout).
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dualcore_remote)

target_sources(app PRIVATE src/main.c)
