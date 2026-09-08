# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(memc_tt_mimir)

target_sources(app PRIVATE src/main.c src/chip_init.c src/mimir_gddr_config.c)

set(SIVAL_SDK ${CMAKE_BINARY_DIR}/grendel-sival-sdk/grendel_sival_sdk)

target_include_directories(app PRIVATE
  ${SIVAL_SDK}/common/include
  ${SIVAL_SDK}/common/include/registers
  ${SIVAL_SDK}/common/include/registers/common
  ${SIVAL_SDK}/common/include/registers/mimir
  ${SIVAL_SDK}/common/include/registers/mimir/smc_view
  ${SIVAL_SDK}/firmware/chiplet_config
  ${SIVAL_SDK}/firmware/drivers/addr_remap/include
  ${SIVAL_SDK}/firmware/drivers/firewall/include
  ${SIVAL_SDK}/firmware/drivers/noc/include
  ${SIVAL_SDK}/firmware/drivers/smc/include
  ${SIVAL_SDK}/firmware/libs/basic_init/include
  ${SIVAL_SDK}/firmware/libs/noc/include)

target_sources(app PRIVATE
  ${SIVAL_SDK}/firmware/drivers/firewall/src/firewall.c
  ${SIVAL_SDK}/firmware/drivers/noc/src/itn.c
  ${SIVAL_SDK}/firmware/drivers/smc/src/smc_init.c
  ${SIVAL_SDK}/firmware/libs/basic_init/src/basic_init.c
  ${SIVAL_SDK}/firmware/libs/noc/src/noc_init.c)
