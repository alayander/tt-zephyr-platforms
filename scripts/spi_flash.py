#!/usr/bin/env python3

# Copyright (c) 2025 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

"""
Standalone SPI flash utility for Blackhole PCIe cards.

Erases or programs the external SPI EEPROM through the STM32 DMC's
JTAG/SWD debug interface using pyocd. By default the script operates on
all ASICs of a board. Use ``--asic-index`` to target a single ASIC
(e.g. on p300 boards which have two).

Examples:
    python spi_flash.py --board-name p100a full_erase
    python spi_flash.py --board-name p100a write_from_ihex preflash.ihex
    python spi_flash.py --board-name p300a full_erase              # erases both ASICs
    python spi_flash.py --board-name p300a --asic-index 0 full_erase   # left ASIC only
    python spi_flash.py --board-name p300a write_from_ihex preflash.ihex
"""

import argparse
import logging
import os
import sys
import time
from pathlib import Path

from pyocd.flash.file_programmer import FileProgrammer
from pyocd.flash.eraser import FlashEraser

import pyocd_utils

logger = logging.getLogger(Path(__file__).stem)


# ---------------------------------------------------------------------------
# Sub-commands
# ---------------------------------------------------------------------------


def _flash_each_asic(args, description, action):
    """Select ASICs per ``args``, then run ``action(session)`` against an
    open pyocd session for each one in turn.

    ``description`` is logged per-ASIC (e.g. "Erasing SPI flash").
    Returns the list of ``(idx, asic)`` operated on, or ``None`` on error.
    """
    board_metadata = pyocd_utils.load_board_metadata()
    selected = pyocd_utils.select_asics(
        board_metadata, args.board_name, args.asic_index
    )
    if selected is None:
        return None

    for idx, asic in selected:
        pyocd_config = pyocd_utils.PYOCD_FLM_PATH / asic["pyocd-config"]
        logger.info("%s on ASIC %d (%s)...", description, idx, asic["name"])

        session = pyocd_utils.get_session(pyocd_config, args.adapter_id, args.no_prompt)
        session.open()
        try:
            action(session)
        finally:
            session.close()

        time.sleep(1)

    return selected


def cmd_full_erase(args):
    """Erase the SPI flash on the specified board (all ASICs or one)."""
    selected = _flash_each_asic(
        args,
        "Erasing SPI flash",
        lambda session: FlashEraser(session, FlashEraser.Mode.CHIP).erase(),
    )
    if selected is None:
        return os.EX_DATAERR

    logger.info(
        "Full erase complete for board %s (%d ASIC(s)).",
        args.board_name,
        len(selected),
    )
    return os.EX_OK


def cmd_write_from_ihex(args):
    """Write an Intel HEX file to SPI flash on the specified board (all ASICs or one)."""
    ihex_file = Path(args.file)
    if not ihex_file.is_file():
        logger.error("Intel HEX file not found: %s", ihex_file)
        return os.EX_NOINPUT

    selected = _flash_each_asic(
        args,
        f"Writing {ihex_file} to SPI flash",
        lambda session: FileProgrammer(session).program(
            str(ihex_file), file_format="hex"
        ),
    )
    if selected is None:
        return os.EX_DATAERR

    logger.info(
        "Write complete for board %s (%d ASIC(s)).",
        args.board_name,
        len(selected),
    )
    return os.EX_OK


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------


def parse_args():
    parser = argparse.ArgumentParser(
        description="SPI flash utility for Blackhole PCIe cards",
        allow_abbrev=False,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--board-name",
        type=str,
        required=True,
        help="Board name as listed in board_metadata.yaml (e.g. p100a, p150a, p300a)",
    )
    parser.add_argument(
        "--asic-index",
        type=int,
        default=None,
        help="Operate on a single ASIC by index (0-based). "
        "If omitted, all ASICs on the board are targeted.",
    )
    pyocd_utils.add_common_args(parser)

    subparsers = parser.add_subparsers(dest="command", required=True)

    # full_erase
    subparsers.add_parser(
        "full_erase",
        help="Erase the entire SPI flash (all ASICs unless --asic-index is given)",
    )

    # write_from_ihex
    write_parser = subparsers.add_parser(
        "write_from_ihex",
        help="Write an Intel HEX file to SPI flash (all ASICs unless --asic-index is given)",
    )
    write_parser.add_argument(
        "file",
        type=str,
        help="Path to the Intel HEX (.ihex) file to program",
    )

    return parser.parse_args()


def main():
    args = parse_args()
    pyocd_utils.setup_logging(args.verbose)

    if args.command == "full_erase":
        return cmd_full_erase(args)
    elif args.command == "write_from_ihex":
        return cmd_write_from_ihex(args)
    else:
        logger.error("Unknown command: %s", args.command)
        return os.EX_USAGE


if __name__ == "__main__":
    sys.exit(main())
