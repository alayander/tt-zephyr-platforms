/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-target ztest for the Tenstorrent Mimir GDDR7 memc driver. The controller
 * device's init runs the full group bring-up (gddr_init: backend install,
 * param-table load, per-tile init + training), so its readiness is the
 * pass/fail signal for the whole sequence.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/ztest.h>

#define MEMC_NODE DT_NODELABEL(memc)

/*
 * The controller device must have come up: device_is_ready() is false if
 * gddr_init() (device init) or an earlier POST_KERNEL SYS_INIT failed.
 */
ZTEST(memc_tt_mimir, test_bringup)
{
	const struct device *memc = DEVICE_DT_GET(MEMC_NODE);

	zassert_true(device_is_ready(memc),
		     "memc controller not ready: GDDR bring-up (gddr_init) failed");
}

ZTEST_SUITE(memc_tt_mimir, NULL, NULL, NULL, NULL, NULL);
