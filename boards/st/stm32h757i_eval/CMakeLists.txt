#
# Copyright (c) 2023 BrainCo Inc.
# Copyright (c) 2025 Foss Analytical A/S
#
# SPDX-License-Identifier: Apache-2.0
#

# Add custom linker section to relocate framebuffers to PSRAM
zephyr_linker_sources_ifdef(CONFIG_LV_Z_VDB_CUSTOM_SECTION
  SECTIONS dc_ram.ld)
