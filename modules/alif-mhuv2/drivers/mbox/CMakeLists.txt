# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alp Lab AB

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_MBOX_ALIF_MHUV2 mbox_alif_mhuv2.c)
