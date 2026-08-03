# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alp Lab AB
#
# Out-of-tree Zephyr module root CMakeLists.txt for alif_se_boot (see
# zephyr/module.yml: build.cmake = "."). Single flat library -- there is
# exactly one translation unit, so this file skips the add_subdirectory()
# indirection used by the sibling alif-mhuv2 module.

# zephyr_include_directories() (NOT zephyr_library_include_directories())
# adds "include/" to the GLOBAL zephyr_interface target, so app code such as
# apps/dualcore_host/src/main.c can `#include <alif_se_boot.h>` without the app
# itself adding this module's path -- matching how Zephyr's own public
# headers are exposed to every library that links zephyr_interface.
zephyr_include_directories(include)

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_ALIF_SE_BOOT src/alif_se_boot.c)
