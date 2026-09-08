/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mimir chip init for DV (tt_memc_chip_init): release tile resets, and open the
 * SMC outbound filter + ITN SMN2ITN firewall -- all before the memc driver
 * initializes GDDR.
 */

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include <basic_init.h>
#include <itn.h>
#include <noc_init.h>

LOG_MODULE_REGISTER(chip_init, CONFIG_LOG_DEFAULT_LEVEL);

#define RESET_UNIT_SS_RESET_COMPLETE_ADDR 0xC0002060u

/* SMN/ITN plus both CCE and GDDR tiles: everything GDDR bring-up reaches. */
#define COLD_RESET_N_MASK                                                                          \
	((uint32_t)(MIMIR_RST_SMN | MIMIR_RST_ITN | MIMIR_RST_CCE0 | MIMIR_RST_CCE0_DEBUG |        \
		    MIMIR_RST_CCE0_TL1 | MIMIR_RST_CCE1 | MIMIR_RST_CCE1_DEBUG |                   \
		    MIMIR_RST_CCE1_TL1 | MIMIR_RST_D2D0 | MIMIR_RST_D2D1 | MIMIR_RST_GDDR0 |       \
		    MIMIR_RST_GDDR1))

/*
 * Bounded poll budget for reset-release completion; on non-convergence fall
 * back to a fixed NOP settle so bring-up still paces (matches the reference SMC
 * bring-up).
 */
#define RESET_COMPLETE_POLL_MAX 500u
#define RESET_SETTLE_SPIN_ITERS 0x40u

#define SMC_OUTBOUND_FILTER_CTRL_0_CONFIG 0xC0016000u
#define SMC_OUTBOUND_FILTER_CTRL_1_CONFIG 0xC0016020u
#define SMC_OUTBOUND_FILTER_CTRL_2_CONFIG 0xC0016040u
#define SMC_OUTBOUND_FILTER_CTRL_3_CONFIG 0xC0016060u
#define FILTER_CONFIG_OPEN                0x01000103u

/* FW-handshake scratch regs */
#define SMC_CTRL_SCRATCH_07_ADDR 0xC0010138u /* firewall window    */
#define SMC_CTRL_SCRATCH_09_ADDR 0xC0010148u /* boot handshake     */
#define SMC_CTRL_SCRATCH_14_ADDR 0xC0010170u /* memory_capacity_gb */
#define SMC_CTRL_SCRATCH_15_ADDR 0xC0010178u /* interleave_mode    */

#define SCRATCH07_FW_START_GB(v)        ((uint64_t)((v) & 0x7Fu))        /* [6:0]   */
#define SCRATCH07_FW_END_GB(v)          ((uint64_t)(((v) >> 7) & 0x7Fu)) /* [13:7]  */
#define SCRATCH15_INTERLEAVE_MODE(v)    (((v) >> 2) & 0x7u)              /* [4:2]   */
#define SCRATCH14_MEMORY_CAPACITY_GB(v) (((v) >> 13) & 0x1Fu)            /* [17:13] */

/* SCRATCH_09 boot-handshake bits the mimir_soc harness polls. */
#define SCRATCH9_FIREWALL_SETUP_DONE BIT(0)
#define SCRATCH9_NOC2AXI_SETUP_DONE  BIT(2)

#define FW_SLOT15_OFFSET 0x1E0u

/* FILTER_CONFIG (smc_cpu_reg.h FILTER_CTRL_FILTER_CONFIG):
 *   read_en[0] write_en[1] addr_mode[4] allow_ns[8] data_bus_width[14:12]
 *   src_id[19:16] group_id[23:20] allow_burst[24]. Reference sets dbw=7,
 *   burst=1, addr_mode=1, r/w=1, src_id/group_id=0.
 */
#define FW_CFG_BASE     (0x1u | 0x2u | 0x10u | 0x7000u | 0x1000000u)
#define FW_CFG_ALLOW_NS (FW_CFG_BASE | 0x100u) /* allow_ns=1 -> 0x01007113 */
#define FW_CFG_SECURE   (FW_CFG_BASE)          /* allow_ns=0 -> 0x01007013 */

/* END_ADDR/START_ADDR are 56-bit fields; full-open end. */
#define FW_ADDR_FULL_OPEN 0x00FFFFFFFFFFFFFFULL

/* Program one firewall filter slot: FILTER_CONFIG, START_ADDR, END_ADDR. */
static inline void fw_prog_slot(uint32_t cfg_addr, uint64_t cfg, uint64_t start, uint64_t end)
{
	sys_write64(cfg, cfg_addr);
	sys_write64(start, cfg_addr + 0x8u);
	sys_write64(end, cfg_addr + 0x10u);
}

/*
 * Program a tile's FIREWALL_0 (allow_ns) and FIREWALL_15 (secure) with a
 * window, mirroring firmware disable_firewall_filters(). Used for both the SMC
 * outbound filter and the ITN tiles.
 */
static inline void fw_prog_tile(uint32_t base, uint64_t start, uint64_t end)
{
	fw_prog_slot(base, FW_CFG_ALLOW_NS, start, end);
	fw_prog_slot(base + FW_SLOT15_OFFSET, FW_CFG_SECURE, start, end);
}

/*
 * Open one mem-tiles entry's ITN firewall windows: MEM tile to the GDDR window
 * (mem_start/mem_end, from the enclosing function), CCE and CCE_CFG full-open.
 */
#define TILE_PROG_FW(node_id, prop, idx)                                                           \
	fw_prog_tile(DT_PROP_BY_PHANDLE_IDX(node_id, prop, idx, mem_filter_base), mem_start,       \
		     mem_end);                                                                     \
	fw_prog_tile(DT_PROP_BY_PHANDLE_IDX(node_id, prop, idx, cce_filter_base), 0x0ULL,          \
		     FW_ADDR_FULL_OPEN);                                                           \
	fw_prog_tile(DT_PROP_BY_PHANDLE_IDX(node_id, prop, idx, cce_cfg_filter_base), 0x0ULL,      \
		     FW_ADDR_FULL_OPEN);

/*
 * Wait until the SMC reset unit reports the domains we just released as
 * complete, before touching SMN/ITN/CCE/GDDR.
 */
static void wait_reset_release_complete(uint32_t mask)
{
	for (uint32_t i = 0; i < RESET_COMPLETE_POLL_MAX; i++) {
		if ((sys_read32(RESET_UNIT_SS_RESET_COMPLETE_ADDR) & mask) == mask) {
			return;
		}
	}

	LOG_WRN("reset-complete did not converge (mask=0x%08x status=0x%08x); "
		"using fixed settle",
		mask, sys_read32(RESET_UNIT_SS_RESET_COMPLETE_ADDR));

	for (volatile uint32_t i = 0; i < RESET_SETTLE_SPIN_ITERS; i++) {
		__asm__ volatile("nop");
	}
}

static int tt_memc_chip_init(void)
{
	/* 1) Release cold+warm reset for SMN/ITN/CCE/GDDR. */
	smc_release_reset(COLD_RESET_N_MASK);
	wait_reset_release_complete(COLD_RESET_N_MASK);

	/*
	 * 2) Open the SMC outbound filter so SoC-space (ITN, GDDR ctrl/PHY) is
	 * reachable. Config-only open on CTRL_0..3 (reference
	 * setup_smc_outbound_filter()).
	 */
	sys_write64(FILTER_CONFIG_OPEN, SMC_OUTBOUND_FILTER_CTRL_0_CONFIG);
	sys_write64(FILTER_CONFIG_OPEN, SMC_OUTBOUND_FILTER_CTRL_1_CONFIG);
	sys_write64(FILTER_CONFIG_OPEN, SMC_OUTBOUND_FILTER_CTRL_2_CONFIG);
	sys_write64(FILTER_CONFIG_OPEN, SMC_OUTBOUND_FILTER_CTRL_3_CONFIG);

	/* 3) Open the ITN SMN2ITN firewall windows. */
	uint32_t s07 = sys_read32(SMC_CTRL_SCRATCH_07_ADDR);
	uint64_t mem_start = SCRATCH07_FW_START_GB(s07) << 30;
	uint64_t mem_end = (SCRATCH07_FW_END_GB(s07) << 30) - 1ULL;
	uint32_t s15 = sys_read32(SMC_CTRL_SCRATCH_15_ADDR);
	uint32_t s14 = sys_read32(SMC_CTRL_SCRATCH_14_ADDR);
	uint32_t interleave_mode = SCRATCH15_INTERLEAVE_MODE(s15);
	uint32_t memory_capacity_gb = SCRATCH14_MEMORY_CAPACITY_GB(s14);

	DT_FOREACH_PROP_ELEM(DT_NODELABEL(memc), mem_tiles, TILE_PROG_FW)

	/* 4) Program ITN interleave mode + GDDR capacity. */
	grendel_err_t err = noc_init_interleave_cfg((noc_interleave_mode_t)interleave_mode);

	if (err != GRENDEL_ERR_OK) {
		LOG_ERR("ITN interleave mode %u rejected: %d", interleave_mode, (int)err);
		return -EINVAL;
	}

	itn_gddr7_memory_address_realignment_cfg((noc_gddr7_realign_t)memory_capacity_gb);

	LOG_INF("Mimir chip init: resets released, outbound filter open, ITN "
		"firewall win[0x%llx..0x%llx], interleave=%u capacity_gb=%u",
		(unsigned long long)mem_start, (unsigned long long)mem_end, interleave_mode,
		memory_capacity_gb);

	/* 5) DV firewall / noc2axi boot handshake. */
	uint32_t s09 = sys_read32(SMC_CTRL_SCRATCH_09_ADDR);

	sys_write32(s09 | SCRATCH9_FIREWALL_SETUP_DONE | SCRATCH9_NOC2AXI_SETUP_DONE,
		    SMC_CTRL_SCRATCH_09_ADDR);
	LOG_INF("DV handshake: asserted SCRATCH_9 firewall/noc2axi setup done");

	return 0;
}

SYS_INIT(tt_memc_chip_init, POST_KERNEL, 0);
