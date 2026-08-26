# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_TT_D2D tt_d2d.c)

# Include closed source SiVal drop.
if(CONFIG_TT_D2D)
  set(SIVAL_EXTDIR ${CMAKE_BINARY_DIR}/grendel-sival-sdk)
  set(SIVAL_BLOBS_TARBALL
      ${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/blobs/grendel-sival-sdk.tar.gz)

  if(NOT EXISTS "${SIVAL_BLOBS_TARBALL}")
    message(FATAL_ERROR
            "The Sival SDK drop is missing:\n  ${SIVAL_BLOBS_TARBALL}\n"
            "It is closed source and distributed separately.")
  endif()

  file(MAKE_DIRECTORY ${SIVAL_EXTDIR})
  execute_process(COMMAND tar -xzf ${SIVAL_BLOBS_TARBALL}
    WORKING_DIRECTORY ${SIVAL_EXTDIR}
  )

  zephyr_include_directories(
    ${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/drivers/d2d/include)
endif()
