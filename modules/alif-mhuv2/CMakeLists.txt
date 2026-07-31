# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alp Lab AB
#
# Out-of-tree Zephyr module root CMakeLists.txt for alif_mhuv2 (see
# zephyr/module.yml: build.cmake = "."). Just descends into drivers/; the
# driver library itself decides whether it builds via CONFIG_MBOX_ALIF_MHUV2.

add_subdirectory(drivers)
